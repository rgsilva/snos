/* SNOS filesystem: a read-only ROM image plus a writable SRAM volume.
 *
 * Two very different media presented as one flat namespace:
 *
 *   ROMFS   packed into the cartridge at build time, at a fixed ROM offset.
 *           Read-only, obviously, and the reason SNOS can ship content at all.
 *   SRAMFS  the 256 KB of battery-backed cartridge SRAM measured by
 *           sram_probe(). Writable, and it survives a reset — this is where a
 *           BASIC program lands when you SAVE it.
 *
 * There are no directories and no seeking backwards while writing. That is
 * enough for what M5 needs, and a filesystem is not the interesting part of
 * this project.
 */
#ifndef SNOS_FS_H
#define SNOS_FS_H

#include <stdint.h>

#define FS_NAME_LEN 16
#define FS_MAX 64

/* ROMFS lives in the last bank of the 1 MB image, leaving $C8-$CE for apps.
 * Its magic is deliberately NOT "SNOS": app_scan() walks these same banks
 * looking for that string, and would happily mistake the filesystem for an
 * application. */
#define ROMFS_ADDR 0xCF0000ul
#define ROMFS_MAGIC0 'S'
#define ROMFS_MAGIC1 'N'
#define ROMFS_MAGIC2 'F'
#define ROMFS_MAGIC3 'S'

/* SRAMFS superblock and directory occupy the first 2 KB; data follows. */
#define SRAMFS_DIR_OFF 16ul
#define SRAMFS_SLOTS 48
#define SRAMFS_ENT_SIZE 32ul
#define SRAMFS_DATA_OFF 2048ul

typedef struct {
  char name[FS_NAME_LEN + 1];
  uint32_t off;  /* ROMFS: bytes from ROMFS_ADDR. SRAMFS: linear SRAM offset. */
  uint32_t size;
  uint8_t ro;    /* 1 = in ROM and immutable */
  uint8_t slot;  /* SRAMFS directory slot, for delete */
} fs_entry_t;

void fs_init(void);
int fs_count(void);
const fs_entry_t *fs_get(int i);
const fs_entry_t *fs_find(const char *name);
uint8_t fs_read_byte(const fs_entry_t *e, uint32_t off);
uint32_t fs_free(void);

/* One file open at a time — enough for SAVE and LOAD, and it keeps the kernel
 * from having to own a descriptor table. */
int fs_open_read(const char *name);
int fs_open_write(const char *name);
int fs_getc(void);              /* -1 at end of file */
int fs_putc(uint8_t c);         /* 0 on success */
void fs_close(void);
uint32_t fs_open_size(void);

int fs_delete(const char *name);

#endif
