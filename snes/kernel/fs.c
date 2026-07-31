#include "fs.h"
#include "proc.h"
/* proc_lock: see the calls below — shell-direct and background-syscall
 * callers share these structures, and only pinning the scheduler makes the
 * multi-store spans atomic for both. */
#include "sram.h"
#include "farmem.h"

#define R8(a) (*(volatile uint8_t *)(a))

static fs_entry_t files[FS_MAX];
static int nfiles;

/* open-file state */
/* Per-process open-file handles (M10). The five fields that used to be the
 * single implicit "current file" — which is exactly why background fs access
 * had to be refused: two processes through one cursor serve each other's
 * bytes. Reads are stateless per byte, so concurrent READERS on separate
 * handles are safe outright. Writing keeps a single system-wide owner: the
 * bump allocator's write-size-on-close design assumes one writer, and a
 * second one gets the same honest -1 a full volume does. */
typedef struct {
  int idx;      /* -1 closed, -2 writing, >= 0 an index into files[] */
  int write;
  uint32_t pos;
  uint32_t start;
  uint8_t slot;
} fs_hand_t;
static fs_hand_t hnd[PROC_MAX];
static uint8_t write_owner = 0xFF; /* proc id holding THE write handle */
#define H (&hnd[proc_current()])

/* Straight 24-bit read. Deliberately NOT far_rd(addr>>16, addr): splitting the
 * address into bank and offset only for far_rd to shift them back together is
 * pointless work, and Calypsi's 32-bit codegen is the last thing to give extra
 * opportunities to. */
static uint8_t rom_read(uint32_t addr) {
  return *(volatile __far uint8_t *)addr;
}

static uint16_t rom_read16(uint32_t addr) {
  return (uint16_t)(rom_read(addr) | ((uint16_t)rom_read(addr + 1) << 8));
}

static uint32_t rom_read32(uint32_t addr) {
  return (uint32_t)rom_read16(addr) | ((uint32_t)rom_read16(addr + 2) << 16);
}

static void name_copy(char *dst, const char *src) {
  int i;
  for (i = 0; i < FS_NAME_LEN; i++) dst[i] = src[i];
  dst[FS_NAME_LEN] = 0;
}

/* Case-insensitive, deliberately. Filenames on this system are conventionally
 * uppercase (that is what the ROM packer emits and what BASIC's SAVE writes),
 * but typing uppercase on a SNES means holding shift on an on-screen keyboard
 * for every character. Making the user's case match the disk's is not a
 * meaningful thing to ask of them. */
static int name_eq(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

/* ------------------------------------------------------------------ ROMFS */

static void romfs_mount(void) {
  uint16_t n, i;
  if (rom_read(ROMFS_ADDR + 0) != ROMFS_MAGIC0 || rom_read(ROMFS_ADDR + 1) != ROMFS_MAGIC1 ||
      rom_read(ROMFS_ADDR + 2) != ROMFS_MAGIC2 || rom_read(ROMFS_ADDR + 3) != ROMFS_MAGIC3)
    return; /* no ROM image in this build */
  n = rom_read16(ROMFS_ADDR + 6);
  for (i = 0; i < n && nfiles < FS_MAX; i++) {
    uint32_t e = ROMFS_ADDR + 16 + (uint32_t)i * 32;
    fs_entry_t *f = &files[nfiles];
    int j;
    for (j = 0; j < FS_NAME_LEN; j++) f->name[j] = (char)rom_read(e + (uint32_t)j);
    f->name[FS_NAME_LEN] = 0;
    f->off = rom_read32(e + 16);
    f->size = rom_read32(e + 20);
    f->ro = 1;
    f->slot = 0;
    nfiles++;
  }
}

/* ----------------------------------------------------------------- SRAMFS */

static uint32_t slot_addr(uint16_t slot) {
  return SRAMFS_DIR_OFF + (uint32_t)slot * SRAMFS_ENT_SIZE;
}

static void sramfs_format(void) {
  uint32_t i;
  sram_wr(0, 'S');
  sram_wr(1, 'N');
  sram_wr(2, 'S');
  sram_wr(3, 'R');
  sram_wr16(4, 1);              /* version */
  sram_wr32(8, SRAMFS_DATA_OFF); /* bump allocator */
  for (i = 0; i < SRAMFS_SLOTS; i++) sram_wr(slot_addr((uint16_t)i), 0);
}

static void sramfs_mount(void) {
  uint16_t s;
  /* SRAM is battery-backed and Mesen persists it to a .srm, so a stale volume
   * from an older build is the normal case, not the exception. Anything that
   * does not carry the magic gets reformatted rather than parsed. */
  if (sram_rd(0) != 'S' || sram_rd(1) != 'N' || sram_rd(2) != 'S' || sram_rd(3) != 'R' ||
      sram_rd16(4) != 1)
    sramfs_format();

  for (s = 0; s < SRAMFS_SLOTS && nfiles < FS_MAX; s++) {
    uint32_t e = slot_addr(s);
    fs_entry_t *f;
    int j;
    if (sram_rd(e) == 0) continue; /* free or deleted */
    f = &files[nfiles];
    for (j = 0; j < FS_NAME_LEN; j++) f->name[j] = (char)sram_rd(e + (uint32_t)j);
    f->name[FS_NAME_LEN] = 0;
    f->off = sram_rd32(e + 16);
    f->size = sram_rd32(e + 20);
    f->ro = 0;
    f->slot = (uint8_t)s;
    nfiles++;
  }
}

void fs_init(void) {
  int i;
  nfiles = 0;
  /* A remount rebuilds files[], so every open handle's index is meaningless
   * afterwards: invalidate them all rather than let one point at whatever
   * reshuffled into its slot. Matches the old single-handle semantics. */
  for (i = 0; i < PROC_MAX; i++) {
    hnd[i].idx = -1;
    hnd[i].write = 0;
  }
  write_owner = 0xFF;
  romfs_mount();
  sramfs_mount();
}

int fs_count(void) { return nfiles; }

const fs_entry_t *fs_get(int i) {
  if (i < 0 || i >= nfiles) return 0;
  return &files[i];
}

/* Look a name up, preferring the SRAM copy.
 *
 * ROMFS is mounted first, so a plain first-match search always returned the ROM
 * entry and a same-named SRAM file was unreachable — you could `download` over
 * a stock file, watch it appear in `ls`, and still `cat` the original forever.
 *
 * The writable copy wins instead, which is what an overlay filesystem does and
 * what someone editing a stock file means. The ROM original is not destroyed,
 * it is shadowed: delete the SRAM copy and it comes back. `ls` marks the pair
 * so this is visible rather than mysterious. */
static int fs_index(const char *name) {
  int i, rom = -1;
  for (i = 0; i < nfiles; i++) {
    if (!name_eq(files[i].name, name)) continue;
    if (!files[i].ro) return i; /* writable copy shadows the ROM one */
    if (rom < 0) rom = i;
  }
  return rom;
}

/* Is this entry hidden by a writable copy of the same name? */
int fs_shadowed(int i) {
  int j;
  if (i < 0 || i >= nfiles || !files[i].ro) return 0;
  for (j = 0; j < nfiles; j++)
    if (!files[j].ro && name_eq(files[j].name, files[i].name)) return 1;
  return 0;
}

const fs_entry_t *fs_find(const char *name) {
  int i = fs_index(name);
  return i < 0 ? 0 : &files[i];
}

uint8_t fs_read_byte(const fs_entry_t *e, uint32_t off) {
  if (off >= e->size) return 0;
  if (e->ro) return rom_read(ROMFS_ADDR + e->off + off);
  return sram_rd(e->off + off);
}

uint32_t fs_free(void) {
  uint32_t used = sram_rd32(8);
  uint32_t total = sram_size();
  if (used >= total) return 0;
  return total - used;
}

/* -------------------------------------------------------------- open file */

int fs_open_read(const char *name) {
  int i = fs_index(name);
  if (i < 0) return -1;
  H->idx = i;
  H->write = 0;
  H->pos = 0;
  return 0;
}

/* Creating a file: take a directory slot, point it at the top of the bump
 * allocator, and stream bytes straight into SRAM. The size is only written back
 * on close, so an interrupted write leaves a zero-length file rather than one
 * that claims bytes it does not have. */
static int fs_delete_locked(const char *name);

int fs_open_write(const char *name) {
  uint16_t s;
  uint32_t alloc;
  int j;
  int rc = -1;

  /* Pinned end to end: the write_owner check, the overwrite-delete's remount
   * and the claim must be one atomic act, or a background SYS_OPEN(write)
   * interleaves with a shell `write` and two writers share one allocator
   * base. */
  proc_lock();

  /* Every refusal must come BEFORE the delete: overwrite semantics on a full
   * volume otherwise destroys the old copy and then fails to create the new
   * one, and "full" is the steady state of a bump allocator with no reclaim.
   * Deleting frees no space (the allocator never rewinds), so the space check
   * is delete-independent; a full directory only blocks names that do not
   * already own a slot, since deleting an existing SRAM copy frees its own. */
  alloc = sram_rd32(8);
  if (alloc >= sram_size()) goto out;
  if (write_owner != 0xFF) goto out; /* one writer system-wide; see hnd */

  for (s = 0; s < SRAMFS_SLOTS; s++)
    if (sram_rd(slot_addr(s)) == 0) break;
  if (s >= SRAMFS_SLOTS) {
    const fs_entry_t *e = fs_find(name);
    if (!e || e->ro) goto out; /* directory full */
  }

  fs_delete_locked(name); /* overwrite semantics */

  for (s = 0; s < SRAMFS_SLOTS; s++)
    if (sram_rd(slot_addr(s)) == 0) break;
  if (s >= SRAMFS_SLOTS) goto out; /* unreachable: guarded above */

  for (j = 0; j < FS_NAME_LEN; j++)
    sram_wr(slot_addr(s) + (uint32_t)j, (uint8_t)(name[j] ? name[j] : 0));
  sram_wr32(slot_addr(s) + 16, alloc);
  sram_wr32(slot_addr(s) + 20, 0);

  H->slot = (uint8_t)s;
  H->start = alloc;
  H->pos = 0;
  H->write = 1;
  H->idx = -2; /* writing: not a directory index */
  write_owner = proc_current();
  rc = 0;
out:
  proc_unlock();
  return rc;
}

int fs_getc(void) {
  const fs_entry_t *e;
  if (H->idx < 0 || H->write) return -1;
  e = &files[H->idx];
  if (H->pos >= e->size) return -1;
  return fs_read_byte(e, H->pos++);
}

int fs_putc(uint8_t c) {
  if (!H->write) return -1;
  if (H->start + H->pos >= sram_size()) return -1;
  sram_wr(H->start + H->pos, c);
  H->pos++;
  return 0;
}

void fs_close(void) {
  proc_lock(); /* commit + remount must not interleave with a bg handle op */
  if (H->write) {
    sram_wr32(slot_addr(H->slot) + 20, H->pos);
    sram_wr32(8, H->start + H->pos); /* commit the bump allocator */
    fs_init();                           /* re-read the directory */
  }
  H->idx = -1;
  if (H->write) write_owner = 0xFF;
  H->write = 0;
  proc_unlock();
}

uint32_t fs_open_size(void) {
  if (H->write) return H->pos;
  if (H->idx < 0) return 0;
  return files[H->idx].size;
}

/* The unpinned body; fs_open_write calls it inside its own pin. */
static int fs_delete_locked(const char *name) {
  int i;
  /* Not while anyone holds the write handle: the remount below reshuffles
   * directory slots, and the writer's slot number must stay true until its
   * close commits. (fs_open_write's own overwrite-delete runs BEFORE it
   * claims the handle, so this does not refuse the overwrite path.) */
  if (write_owner != 0xFF) return -1;
  /* fs_index prefers the writable copy, so deleting a shadowed name removes the
   * SRAM one and reveals the ROM original again rather than failing. */
  {
    i = fs_index(name);
    if (i >= 0) {
      if (files[i].ro) return -1; /* ROM is not negotiable */
      sram_wr(slot_addr(files[i].slot), 0);
      fs_init();
      return 0;
    }
  }
  return -1;
}


int fs_delete(const char *name) {
  int rc;
  /* Pinned: the remount rebuilds files[] and invalidates handles, and a
   * background fs syscall dereferencing files[] mid-rebuild reads a torn
   * entry. */
  proc_lock();
  rc = fs_delete_locked(name);
  proc_unlock();
  return rc;
}

/* ---- straight from storage into VRAM ------------------------------------
 *
 * Cartridge SRAM sits on the A-bus and VRAM is reachable through the B-bus at
 * $2118, so image data never has to travel through WRAM or through C at all.
 * The per-byte path (fs_getc) measures around 30 KB/s, which is 1.4 seconds for
 * a 43 KB picture; DMA moves the same bytes in a few frames.
 *
 * The caller must have the screen in forced blank and must have set the VRAM
 * address and $2115 already — this only supplies the bytes.
 *
 * Each DMA is clipped to the end of its 8 KB SRAM window (or its ROM bank),
 * because the DMA controller increments the A-bus address WITHOUT carrying into
 * the bank: a transfer that ran past $7FFF would wrap to $6000 of the same
 * window and quietly repeat 8 KB of the picture.
 */
static void dma_block(uint16_t bank, uint16_t addr, uint16_t bytes) {
  R8(0x4300) = 0x01; /* A->B, two registers ($2118/$2119) */
  R8(0x4301) = 0x18;
  R8(0x4302) = (uint8_t)addr;
  R8(0x4303) = (uint8_t)(addr >> 8);
  R8(0x4304) = (uint8_t)bank;
  R8(0x4305) = (uint8_t)bytes;
  R8(0x4306) = (uint8_t)(bytes >> 8);
  R8(0x420B) = 0x01;
}

uint16_t fs_read_vram(uint16_t count) {
  const fs_entry_t *e;
  uint32_t remain, pos;
  uint16_t moved = 0;

  if (H->idx < 0 || H->write) return 0;
  e = &files[H->idx];
  pos = H->pos;
  if (pos >= e->size) return 0;
  remain = e->size - pos;
  if ((uint32_t)count < remain) remain = count;

  while (remain) {
    uint16_t bank, addr, chunk;
    uint32_t space;

    if (e->ro) {
      uint32_t phys = ROMFS_ADDR + e->off + pos;
      bank = (uint16_t)(phys >> 16);
      addr = (uint16_t)phys;
      space = 0x10000ul - (uint32_t)addr;
    } else {
      uint32_t off = e->off + pos;
      bank = (uint16_t)(SRAM_BANK0 + (uint16_t)(off >> SRAM_WINDOW_BITS));
      addr = (uint16_t)(0x6000u + ((uint16_t)off & (uint16_t)(SRAM_WINDOW_SIZE - 1)));
      space = 0x8000ul - (uint32_t)addr;
    }
    if (space > remain) space = remain;
    if (space > 0x2000ul) space = 0x2000ul;
    chunk = (uint16_t)space;

    dma_block(bank, addr, chunk);
    pos += chunk;
    remain -= chunk;
    moved = (uint16_t)(moved + chunk);
  }
  H->pos = pos;
  return moved;
}

/* A process died (exit or kill): release its fs state. An uncommitted write
 * is abandoned, which the on-close-commit design makes safe by construction:
 * the slot never had its size written, so the file simply does not exist. */
void fs_reap(uint8_t proc) {
  if (proc >= PROC_MAX) return;
  if (write_owner == proc && hnd[proc].write) {
    /* The open claimed a directory slot (name written, size 0) that only
     * fs_close would have finalised. sramfs_mount mounts any slot with a
     * non-zero first byte, so without this the dead writer leaves a ghost
     * zero-length file occupying a slot until someone notices and rms it. */
    sram_wr(slot_addr(hnd[proc].slot), 0);
    write_owner = 0xFF;
  } else if (write_owner == proc) {
    write_owner = 0xFF;
  }
  hnd[proc].idx = -1;
  hnd[proc].write = 0;
}
