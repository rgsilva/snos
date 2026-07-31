/* Test mailbox polled by the Mesen Lua harness, at the top of WRAM bank $7E
 * ($7E:F000-$7E:FFFF, out of bank 0, which belongs to the stack and direct
 * pages). Layout, all offsets from MB_BASE:
 *   +$000 u16 magic          0xCAFE = ROM alive
 *   +$002 u16 status         0 running, 1 all passed, 0xDEAD panic
 *   +$004 u16 write_index    stdout, SNES writes; monotonic, wraps at 65536
 *   +$006 u16 in_windex      stdin, harness writes; monotonic
 *   +$008 u16 in_rindex      stdin, SNES writes; monotonic (lets the
 *                            harness compute free space)
 *   +$00A u16 flips          debug: raw pad word (see input.c)
 *   +$00C u16 con state      console scroll state (see console.c)
 *   +$00E u16 break          keyboard break, kbdbridge writes (see traps.c)
 *   +$010 u8[3552] ring      stdout ring buffer (+$010..+$DEF)
 *   +$E00 u8[512] in_ring    stdin ring buffer (+$E00..+$FFF)
 * Index words are only ever written by one side, and the harness runs with
 * the CPU paused (end-of-frame Lua callback), so no torn reads either way.
 * (This header began life in snesmac; its first comment still described that
 * project's bank-$7F layout and MicroPython heap, none of which ever existed
 * here — the constants below were always the truth.) */
#ifndef SNES_MAILBOX_H
#define SNES_MAILBOX_H

#include <stdint.h>

#define MB_MAGIC_ALIVE 0xCAFEu
#define MB_STATUS_RUNNING 0u
#define MB_STATUS_PASS 1u
#define MB_STATUS_PANIC 0xDEADu

/* Mailbox base: top of WRAM bank $7E. (A vestigial MAC_SNES branch from
 * snesmac lived here for months; no SNOS build ever defined it.) */
#define MB_BASE 0x7EF000ul
#define MB_RING_SIZE 3552u
#define MB_IN_RING_SIZE 512u

#define MB_MAGIC (*(volatile __far uint16_t *)(MB_BASE + 0x000))
#define MB_STATUS (*(volatile __far uint16_t *)(MB_BASE + 0x002))
#define MB_WINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x004))
#define MB_IN_WINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x006))
#define MB_IN_RINDEX (*(volatile __far uint16_t *)(MB_BASE + 0x008))
#define MB_FLIPS (*(volatile __far uint16_t *)(MB_BASE + 0x00A))
/* Was a 32-bit counter in snesmac; here 0x00C is the console scroll state
 * (console.c) and 0x00E is the break flag, so it is 16 bits wide. */
#define MB_VMOPS (*(volatile __far uint16_t *)(MB_BASE + 0x00C))
/* Attention/break, written by the host bridge when F12 is pressed.
 *
 * Deliberately NOT a byte in the stdin ring. A hung application never calls
 * kbd_getchar(), so anything queued in the stream is read by nobody and the one
 * situation a break exists for is exactly the one where it cannot arrive. This
 * is polled from NMI instead, which the kernel owns unconditionally and which
 * an application cannot prevent from running — the same reasoning that puts the
 * START+SELECT kill switch there. */
#define MB_BREAK (*(volatile __far uint16_t *)(MB_BASE + 0x00E))

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
