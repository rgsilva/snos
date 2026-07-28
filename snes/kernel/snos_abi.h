/* SNOS system-call ABI — the contract between the kernel and applications.
 *
 * This header is included by BOTH the kernel and every app. Apps are linked
 * separately from the kernel (each into its own ROM bank), so everything here
 * has to be either a compile-time constant or a fixed address. Nothing in this
 * file may depend on where a linker chose to put something.
 *
 * ---------------------------------------------------------------- mechanism
 *
 * A system call is `COP #n`. That is Nintendo's own choice — SFX-DOS (1991)
 * used exactly this (`cop 15h` init, `cop 21h` load) — and it is a good one:
 * the vector at $00:FFE4 is otherwise unused on the SNES, the instruction is
 * two bytes at the call site against four for a JSL, and the round trip is
 * about 15 cycles. BRK is reserved for the debugger so the two can never
 * collide over signature space.
 *
 * Signatures $00-$7F only. $80-$FF are reserved by WDC for coprocessor use.
 *
 * ------------------------------------------------------------------- args
 *
 * Argument 0 travels in the accumulator, because that is where a Calypsi C
 * call already puts it and because a hand-written asm app finds it natural.
 *
 * Everything else travels in a FIXED parameter block at $7E1A00. It does not
 * use X and Y, and it does not use Calypsi's own multi-argument convention —
 * that convention passes argument 2 in a compiler-internal direct-page slot
 * (`_Dp`), which is an implementation detail of one version of one compiler
 * and absolutely not something to build an ABI on.
 *
 * $7E1A00 is the WRAM mirror of $00:1A00, so the block is reachable by long
 * addressing from any bank. Apps run with DB pointing at their own ROM bank
 * (that is what makes them bank-position-independent), so they cannot reach
 * bank $00 with absolute addressing and must use the long form.
 */
#ifndef SNOS_ABI_H
#define SNOS_ABI_H

#include <stdint.h>

#define SNOS_ABI_VERSION 1

/* ----------------------------------------------------------- call numbers */
/* $00-$0F  process */
#define SYS_EXIT 0x00     /* A = exit code. Does not return. */
#define SYS_VERSION 0x01  /* -> kernel ABI version */
#define SYS_YIELD 0x02    /* give the kernel a frame (housekeeping, OSK) */
/* $10-$1F  console */
#define SYS_PUTC 0x10     /* A = character */
#define SYS_PUTS 0x11     /* p0 = offset, p1 = bank; NUL-terminated */
#define SYS_GETC 0x12     /* -> ASCII, blocking */
#define SYS_CLS 0x13
#define SYS_READLINE 0x14 /* p0/p1 = buffer, A = size -> length */
#define SYS_PUTU 0x15     /* A = value, unsigned decimal */
#define SYS_PUTHEX 0x16   /* A = value, 4 hex digits */
/* $20-$2F  input */
#define SYS_PAD 0x20 /* -> held-button bitmask (PAD_* below) */
/* $50-$5F  time */
#define SYS_FRAMES 0x50   /* -> free-running 60 Hz frame counter */
#define SYS_WAITVBL 0x51
/* $70-$7F  debug */
#define SYS_TRACE 0x70 /* p0/p1 = string -> the test mailbox */

#define SYS_ERR 0xFFFFu /* generic failure return */

/* ------------------------------------------------------- parameter block */

#define SNOS_ARGS_ADDR 0x7E1A00ul

typedef struct {
  uint16_t p0;
  uint16_t p1;
  uint16_t p2;
  uint16_t p3;
} snos_args_t;

#define SNOS_ARGS (*(volatile __far snos_args_t *)SNOS_ARGS_ADDR)

/* Convenience for the common "pointer argument" case: a 24-bit pointer split
 * into offset and bank. */
#define SNOS_SET_PTR(off, bank)                                                \
  do {                                                                         \
    SNOS_ARGS.p0 = (uint16_t)(off);                                            \
    SNOS_ARGS.p1 = (uint16_t)(bank);                                           \
  } while (0)

/* ------------------------------------------------------------ call stubs */
/* One `COP #n; RTL` thunk each, in syscall.s. Deliberately assembly: it is the
 * one place the register-level ABI is spelled out, and it stays valid whatever
 * the C compiler decides to do. */

uint16_t sys_exit(uint16_t code);
uint16_t sys_version(void);
uint16_t sys_yield(void);
uint16_t sys_putc(uint16_t c);
uint16_t sys_puts_raw(void); /* args in the parameter block */
uint16_t sys_getc(void);
uint16_t sys_cls(void);
uint16_t sys_readline_raw(uint16_t size);
uint16_t sys_putu(uint16_t v);
uint16_t sys_puthex(uint16_t v);
uint16_t sys_frames(void);
uint16_t sys_waitvbl(void);
uint16_t sys_pad(void);
uint16_t sys_trace_raw(void);

/* Button bits as returned by SYS_PAD (the auto-joypad read layout). */
#define PAD_B 0x8000u
#define PAD_Y 0x4000u
#define PAD_SELECT 0x2000u
#define PAD_START 0x1000u
#define PAD_UP 0x0800u
#define PAD_DOWN 0x0400u
#define PAD_LEFT 0x0200u
#define PAD_RIGHT 0x0100u
#define PAD_A 0x0080u
#define PAD_X 0x0040u
#define PAD_L 0x0020u
#define PAD_R 0x0010u

/* Pointer arguments: stash a 24-bit pointer in the parameter block. */
static void snos_set_ptr(const char *s) {
  uint32_t p = (uint32_t)s;
  SNOS_ARGS.p0 = (uint16_t)p;
  SNOS_ARGS.p1 = (uint16_t)(p >> 16);
}

static uint16_t sys_puts(const char *s) {
  snos_set_ptr(s);
  return sys_puts_raw();
}

static uint16_t sys_trace(const char *s) {
  snos_set_ptr(s);
  return sys_trace_raw();
}

#endif
