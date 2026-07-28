#include "fs.h"
#include "sram.h"
#include "farmem.h"

static fs_entry_t files[FS_MAX];
static int nfiles;

/* open-file state */
static int open_idx = -1;
static int open_write;
static uint32_t open_pos;
static uint32_t open_start;
static uint8_t open_slot;

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
  nfiles = 0;
  open_idx = -1;
  romfs_mount();
  sramfs_mount();
}

int fs_count(void) { return nfiles; }

const fs_entry_t *fs_get(int i) {
  if (i < 0 || i >= nfiles) return 0;
  return &files[i];
}

const fs_entry_t *fs_find(const char *name) {
  int i;
  for (i = 0; i < nfiles; i++)
    if (name_eq(files[i].name, name)) return &files[i];
  return 0;
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
  int i;
  for (i = 0; i < nfiles; i++) {
    if (name_eq(files[i].name, name)) {
      open_idx = i;
      open_write = 0;
      open_pos = 0;
      return 0;
    }
  }
  return -1;
}

/* Creating a file: take a directory slot, point it at the top of the bump
 * allocator, and stream bytes straight into SRAM. The size is only written back
 * on close, so an interrupted write leaves a zero-length file rather than one
 * that claims bytes it does not have. */
int fs_open_write(const char *name) {
  uint16_t s;
  uint32_t alloc;
  int j;

  fs_delete(name); /* overwrite semantics */

  for (s = 0; s < SRAMFS_SLOTS; s++)
    if (sram_rd(slot_addr(s)) == 0) break;
  if (s >= SRAMFS_SLOTS) return -1; /* directory full */

  alloc = sram_rd32(8);
  if (alloc >= sram_size()) return -1;

  for (j = 0; j < FS_NAME_LEN; j++)
    sram_wr(slot_addr(s) + (uint32_t)j, (uint8_t)(name[j] ? name[j] : 0));
  sram_wr32(slot_addr(s) + 16, alloc);
  sram_wr32(slot_addr(s) + 20, 0);

  open_slot = (uint8_t)s;
  open_start = alloc;
  open_pos = 0;
  open_write = 1;
  open_idx = -2; /* writing: not a directory index */
  return 0;
}

int fs_getc(void) {
  const fs_entry_t *e;
  if (open_idx < 0 || open_write) return -1;
  e = &files[open_idx];
  if (open_pos >= e->size) return -1;
  return fs_read_byte(e, open_pos++);
}

int fs_putc(uint8_t c) {
  if (!open_write) return -1;
  if (open_start + open_pos >= sram_size()) return -1;
  sram_wr(open_start + open_pos, c);
  open_pos++;
  return 0;
}

void fs_close(void) {
  if (open_write) {
    sram_wr32(slot_addr(open_slot) + 20, open_pos);
    sram_wr32(8, open_start + open_pos); /* commit the bump allocator */
    fs_init();                           /* re-read the directory */
  }
  open_idx = -1;
  open_write = 0;
}

uint32_t fs_open_size(void) {
  if (open_write) return open_pos;
  if (open_idx < 0) return 0;
  return files[open_idx].size;
}

int fs_delete(const char *name) {
  int i;
  for (i = 0; i < nfiles; i++) {
    if (name_eq(files[i].name, name)) {
      if (files[i].ro) return -1; /* ROM is not negotiable */
      sram_wr(slot_addr(files[i].slot), 0);
      fs_init();
      return 0;
    }
  }
  return -1;
}
