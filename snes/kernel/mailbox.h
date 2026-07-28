// Test mailbox polled by the Mesen Lua harness, at the top of WRAM bank $7F
// (out of bank 0, which belongs to the C stack — mp_init alone needs ~6KB
// and an overflow once chewed straight through the old $7E0100 mailbox).
// Layout:
//   $7FE000 u16 magic        0xCAFE = ROM alive
//   $7FE002 u16 status       0 running, 1 all passed, 0xDEAD panic
//   $7FE004 u16 write_index  stdout, SNES writes; monotonic, wraps at 65536
//   $7FE006 u16 in_windex    stdin, harness writes; monotonic
//   $7FE008 u16 in_rindex    stdin, SNES writes; monotonic (lets the
//                            harness compute free space)
//   $7FE010 u8[7664] ring    stdout ring buffer ($7FE010-$7FFDFF)
//   $7FFE00 u8[512] in_ring  stdin ring buffer ($7FFE00-$7FFFFF)
// The GC heap occupies the rest of bank $7F below ($7F0000-$7FDFFF).
// Index words are only ever written by one side, and the harness runs with
// the CPU paused (end-of-frame Lua callback), so no torn reads either way.
#ifndef SNES_MAILBOX_H
#define SNES_MAILBOX_H

#include <stdint.h>

#define MB_MAGIC_ALIVE 0xCAFEu
#define MB_STATUS_RUNNING 0u
#define MB_STATUS_PASS 1u
#define MB_STATUS_PANIC 0xDEADu

/* Mailbox base. Plain builds: top of WRAM bank $7E ($7E:F000-$7E:FFFF).
   Mac builds (-DMAC_SNES): WRAM $7E2000-$7FFFFF IS the Mac's RAM (PERF.md
   item H), so the mailbox moves to cart-SRAM window 18 — bank $B2:6000
   (fast mirror), SRAM byte offset $24000, above the Mac's 16 windows. The
   Lua side reads it as snesSaveRam at 0x24000. Same internal layout. */
#ifdef MAC_SNES
#  define MB_BASE 0xB26000ul
#else
#  define MB_BASE 0x7EF000ul
#endif
#define MB_RING_SIZE 3552u
#define MB_IN_RING_SIZE 512u

#define MB_MAGIC (*(volatile __far uint16_t *)(MB_BASE + 0x000))
#define MB_STATUS (*(volatile __far uint16_t *)(MB_BASE + 0x002))
#define MB_WINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x004))
#define MB_IN_WINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x006))
#define MB_IN_RINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x008))
#define MB_FLIPS (*(volatile __far uint16_t *)(MB_BASE + 0x00A))
#define MB_VMOPS (*(volatile __far uint32_t *)(MB_BASE + 0x00C))
#define MB_RING ((volatile __far uint8_t *)(MB_BASE + 0x010))
#define MB_IN_RING ((volatile __far uint8_t *)(MB_BASE + 0xE00))

void mb_init(void);
void mb_putc(char c);
void mb_puts(const char *s); // writes the string, no newline added
void mb_puthex32(uint32_t v); // 8 hex digits, no prefix
int mb_getc_nonblock(void); // next stdin byte, or -1 if none pending
char mb_getc(void); // blocking stdin read (spins; harness feeds per frame)
void mb_finish(uint16_t status); // sets status and spins forever

#endif
