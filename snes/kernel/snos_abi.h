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

/* Version 2 added the four calls a FULL-SCREEN application needs. Until then
 * the console was a stream device to an app — text in, scrolling out — with no
 * way to address a position, discover the usable height, read input without
 * blocking, or get the d-pad away from the on-screen keyboard. An editor is not
 * merely awkward without those; it is impossible.
 *
 * Version 3 added the network calls. The abstraction handed to an application
 * is a STREAM and nothing else: the gateway terminates TLS and speaks HTTP, so
 * FETCH and a future raw OPEN differ only in how the stream comes into being.
 * An application never sees an IP packet, a TLS record or a header block. */
#define SNOS_ABI_VERSION 3

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
/* ABI 2. Writes a string at an absolute screen position, clipped to the row:
 * it never moves the cursor and never scrolls. That last part is the point —
 * SYS_PUTS landing on the last column of the last row scrolls the display,
 * which quietly destroys any full-screen layout. */
#define SYS_PUTS_AT 0x17 /* p0/p1 = string, A = (row << 8) | col */
/* ABI 2. -> (usable text rows << 8) | columns. The usable height is NOT fixed:
 * it drops from 28 to 22 when the on-screen keyboard appears, underneath a
 * running application, so anything drawing a full screen has to ask. */
#define SYS_SCREEN 0x18
/* $20-$2F  input */
#define SYS_PAD 0x20 /* -> held-button bitmask (PAD_* below) */
/* ABI 2. Non-blocking counterpart to SYS_GETC: returns SYS_ERR when nothing is
 * pending, so an app can poll the keyboard and the d-pad in one loop instead of
 * blocking on one of them. */
#define SYS_POLLKEY 0x21
/* ABI 2. Show or hide the on-screen keyboard. An app needs this because the
 * keyboard OWNS the d-pad while it is visible (that is how you move between
 * keys), so a menu or a file picker cannot be navigated until it is out of the
 * way. A = 0 hide, 1 show, anything else queries. -> 1 if visible. */
#define SYS_OSK 0x22
/* $30-$3F  files. One file open at a time — enough for SAVE and LOAD. */
#define SYS_OPEN 0x30   /* p0/p1 = name, A = 0 read / 1 write -> 0 or SYS_ERR */
#define SYS_GETC_F 0x31 /* -> byte, or SYS_ERR at end of file */
#define SYS_PUTC_F 0x32 /* A = byte -> 0 or SYS_ERR */
#define SYS_CLOSE 0x33
#define SYS_FSIZE 0x34  /* -> size of the open file (low 16 bits) */
#define SYS_DIR 0x35    /* A = index, p0/p1 = 17-byte buffer -> 1 ok, 0 end */
#define SYS_UNLINK 0x36 /* p0/p1 = name -> 0 or SYS_ERR */
/* $50-$5F  time */
#define SYS_FRAMES 0x50   /* -> free-running 60 Hz frame counter */
#define SYS_WAITVBL 0x51
/* $60-$6F  network. Streams via a gateway; see kernel/net.h for why the
 * gateway does TLS and HTTP and the SNES does not. */
#define SYS_NET_UP 0x60    /* -> 1 if a gateway is attached */
#define SYS_NET_FETCH 0x61 /* p0/p1 = URL -> channel, or SYS_ERR */
#define SYS_NET_READ 0x62  /* p0/p1 = buffer, p2 = channel, A = max -> count */
#define SYS_NET_STATE 0x63 /* A = channel -> NET_ST_*; p0 = HTTP status */
#define SYS_NET_CLOSE 0x64 /* A = channel */
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
uint16_t sys_puts_at_raw(uint16_t rowcol); /* ABI 2 */
uint16_t sys_screen(void);
uint16_t sys_pollkey(void);
uint16_t sys_osk(uint16_t mode);
uint16_t sys_open(uint16_t mode);
uint16_t sys_getc_f(void);
uint16_t sys_putc_f(uint16_t c);
uint16_t sys_close(void);
uint16_t sys_fsize(void);
uint16_t sys_dir(uint16_t index);
uint16_t sys_unlink(void);
uint16_t sys_trace_raw(void);
uint16_t sys_net_up(void);
uint16_t sys_net_fetch_raw(void); /* URL in the parameter block */
uint16_t sys_net_read_raw(uint16_t max);
uint16_t sys_net_state(uint16_t ch);
uint16_t sys_net_close(uint16_t ch);

/* Console character set, as far as an application is concerned.
 *
 * $80-$FF are the inverse-video twins of $00-$7F — Mode 5 spends both its BG
 * layers on text columns, so there is no spare layer to highlight with and
 * inverting the glyph is how selection is shown.
 *
 * $01-$06 are box-drawing glyphs, which is what makes a framed menu possible
 * without an application shipping its own font. */
/* Arrow keys, as delivered by kbd_getchar()/SYS_GETC alongside ASCII.
 *
 * Deliberately NOT ANSI escape sequences. ESC already means "open the menu" in
 * the editor, so `ESC [ A` would need a timeout to tell an arrow from a bare
 * ESC — and timeout-based key parsing is the sort of thing that passes every
 * test and feels wrong in the hand. Both ends of this link are ours, so
 * unambiguous single codes are simply better. DC1-DC4 are otherwise unused and
 * the line editor already discards everything below $20.
 *
 * The on-screen keyboard emits these from the d-pad WHEN IT IS HIDDEN, so an
 * application cannot tell a controller from a host keyboard — which is the
 * entire point of the input abstraction. */
#define SNOS_KEY_UP 0x11
#define SNOS_KEY_DOWN 0x12
#define SNOS_KEY_LEFT 0x13
#define SNOS_KEY_RIGHT 0x14

#define SNOS_INVERSE 0x80u
#define SNOS_BOX_H 1
#define SNOS_BOX_V 2
#define SNOS_BOX_TL 3
#define SNOS_BOX_TR 4
#define SNOS_BOX_BL 5
#define SNOS_BOX_BR 6

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

/* Stream states, mirroring kernel/net.h. An application loops until the state
 * stops being OPEN; EOF is reported only once the buffered bytes are drained,
 * so nothing that already arrived is ever abandoned. */
#define SNOS_NET_FREE 0
#define SNOS_NET_OPENING 1
#define SNOS_NET_OPEN 2
#define SNOS_NET_EOF 3
#define SNOS_NET_ERROR 4

static uint16_t sys_net_fetch(const char *url) {
  snos_set_ptr(url);
  return sys_net_fetch_raw();
}

static uint16_t sys_net_read(uint16_t ch, void *buf, uint16_t max) {
  snos_set_ptr((const char *)buf);
  SNOS_ARGS.p2 = ch;
  return sys_net_read_raw(max);
}

static uint16_t sys_puts_at(int row, int col, const char *s) {
  snos_set_ptr(s);
  return sys_puts_at_raw((uint16_t)(((uint16_t)row << 8) | (uint8_t)col));
}

#endif
