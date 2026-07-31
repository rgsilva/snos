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
/* Version 4 added launch arguments, so `run edit motd.txt` can hand the file
 * name to the application. There was no mechanism at all before it: every app
 * started knowing nothing about why. */
/* Version 5 added what a graphics application needs: a block file read, and a
 * way to take the PPU away from the console and give it back. Reading 43 KB of
 * image through SYS_GETC_F would be 43000 traps; and Mode 3 tile data for a
 * full screen is 57 KB of the 64 KB of VRAM, so the console's font and tilemaps
 * cannot coexist with it and something has to arbitrate. */
#define SNOS_ABI_VERSION 8

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
/* ABI 5. Hand the PPU to the application, or give it back.
 *
 * A full-screen image needs essentially all of VRAM, so it necessarily destroys
 * the console's font and tilemaps. While acquired, the kernel stops touching
 * VRAM entirely (con_flush becomes a no-op) and the on-screen keyboard is
 * hidden, which also disables the HDMA split that would otherwise rewrite the
 * BG registers every frame and tear a Mode 3 display apart.
 *
 * Releasing rebuilds the console from scratch. Scrollback does not survive,
 * which is honest: the pixels were genuinely overwritten.
 * A = 1 acquire, 0 release. */
#define SYS_GFX 0x19
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
/* ABI 5. Block read into a far buffer -> bytes actually read, 0 at end of file.
 * SYS_GETC_F is one trap per byte, which is fine for a BASIC program and
 * absurd for a 43 KB image. */
#define SYS_READ_F 0x37 /* p0/p1 = buffer, A = max -> count */
/* ABI 5. Stream the open file straight into VRAM by DMA, never touching WRAM
 * or C. Cartridge SRAM is on the A-bus and VRAM answers on the B-bus, so the
 * bytes go directly; the per-byte path manages about 30 KB/s, which is 1.4
 * seconds for a 43 KB picture against a few frames for this.
 * The caller must have forced blank and set the VRAM address first. */
#define SYS_READ_VRAM 0x38 /* A = byte count -> bytes moved */
/* ABI 6. Free space on the SRAM volume, in KILOBYTES — 254 KB fits a uint16
 * where the byte count would not. An application that streams to a file needs
 * this before it starts: discovering the volume is full after transferring for
 * a minute wastes the minute. */
#define SYS_FS_FREE 0x39
/* $50-$5F  time */
#define SYS_FRAMES 0x50   /* -> free-running 60 Hz frame counter */
#define SYS_WAITVBL 0x51
/* $60-$6F  network. Streams via a gateway; see kernel/net.h for why the
 * gateway does TLS and HTTP and the SNES does not. */
#define SYS_NET_UP 0x60    /* -> 1 if a gateway is attached */
#define SYS_NET_FETCH 0x61 /* p0/p1 = URL -> channel, or SYS_ERR */
#define SYS_NET_READ 0x62  /* p0/p1 = buffer, p2 = channel, A = max -> count */
/* A = channel -> NET_ST_*; p0 = HTTP status; p1 = 1 if the gateway converted
 * the body to Mode 3 tile data (it decides from the Content-Type, so nothing
 * has to be declared in advance). */
#define SYS_NET_STATE 0x63
#define SYS_NET_CLOSE 0x64 /* A = channel */

/* States returned by SYS_NET_STATE. These are ABI, not kernel internals — an
 * application cannot interpret the return value without them, and net.h is not
 * on an application's include path. Kept numerically identical to net.h. */
#define NET_ST_FREE 0
#define NET_ST_OPENING 1
#define NET_ST_OPEN 2
#define NET_ST_EOF 3
#define NET_ST_ERROR 4
/* ABI 7. Bound to a port, waiting for somebody to connect. A listening channel
 * cycles LISTEN -> OPEN -> EOF -> LISTEN without rebinding, so a server app is
 * a loop rather than a fresh listen per request — and the port is not released
 * between connections, where a straggler could steal it. */
#define NET_ST_LISTEN 5
/* ABI 5. Exactly SYS_NET_FETCH, except the gateway converts the resource to
 * Mode 3 tile data first. Same stream, same states, same reads. */
#define SYS_NET_IMAGE 0x65 /* p0/p1 = URL -> channel, or SYS_ERR */

/* ABI 6. Link counters, so a stalled transfer can say which half of the system
 * stalled. A gateway that logs a successful fetch while the SNES reports a
 * timeout is not a contradiction — it means the reply never crossed the link —
 * but without these numbers the two ends look equally plausible and you end up
 * guessing. Returns frames received; p0 = resends, p1 = bad checksums. */
#define SYS_NET_STATS 0x66

/* ABI 7. The SNES can send now, not only ask. Until these existed every byte it
 * put on the wire was a control record, which is enough to fetch a URL and
 * nothing else — no IRC, no echo server, no HTTP response. */
#define SYS_NET_CONNECT 0x67 /* p0/p1 = "host:port" -> channel, or SYS_ERR */
#define SYS_NET_WRITE 0x68   /* p0/p1 = buffer, p2 = channel, A = len -> taken */
#define SYS_NET_WSPACE 0x69  /* A = channel -> bytes acceptable right now */
#define SYS_NET_LISTEN 0x6A  /* A = port -> channel, or SYS_ERR */
/* A = channel -> 0, or SYS_ERR while the response is still going out. Retry:
 * hanging up with bytes unsent would truncate what the client receives. */
#define SYS_NET_HANGUP 0x6B

/* $6C/$6D were ABI 8's browser calls (SYS_WEB_OPEN / SYS_WEB_OP), retired
 * when `web` was deleted — vnc superseded it. The numbers stay reserved: an
 * old binary trapping into them must fail as an unknown syscall, not alias
 * onto whatever gets defined next. */
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

/* ------------------------------------------------------ launch arguments */
/* ABI 4. Whatever followed the application name on the shell's command line,
 * NUL-terminated, at a fixed address in the same reserved bank-$00 block as the
 * parameter block. Read directly rather than through a syscall, exactly as
 * SNOS_ARGS is — the convention already exists and this burns no call number.
 *
 * The kernel CLEARS it on every launch. An app that is given no argument must
 * see an empty string, not whatever the previous app was launched with; stale
 * state read as fresh is the same class of bug as the un-zeroed BSS that made
 * the editor exit instantly. */
#define SNOS_ARGV_ADDR 0x7E1A40ul
#define SNOS_ARGV_MAX 128

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
uint16_t sys_net_image_raw(void);
uint16_t sys_net_stats(void);
uint16_t sys_net_connect_raw(void);
uint16_t sys_net_write_raw(uint16_t len);
uint16_t sys_net_wspace(uint16_t ch);
uint16_t sys_net_listen(uint16_t port);
uint16_t sys_net_hangup(uint16_t ch);
/* Wrappers, so callers do not each re-do the pointer marshalling. */
#define sys_net_connect(hp) (snos_set_ptr(hp), sys_net_connect_raw())
#define sys_net_write(ch, buf, len) \
  (snos_set_ptr(buf), SNOS_ARGS.p2 = (ch), sys_net_write_raw(len))
uint16_t sys_gfx(uint16_t on);
uint16_t sys_read_f_raw(uint16_t max);
uint16_t sys_fs_free(void);
uint16_t sys_read_vram(uint16_t count);

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
 * unambiguous single codes are simply better.
 *
 * $1C-$1F (the separator codes) and NOT $11-$14. The DC1-DC4 range looks
 * unused right up until you notice it is precisely ^Q ^R ^S ^T — and the editor
 * already binds ^Q for Quit and ^S for Save, so arrow-up and Quit would have
 * been the same byte. Nothing binds ^\ ^] ^^ ^_, and $1C-$1F is the range 8-bit
 * machines traditionally used for cursor movement anyway.
 *
 * The on-screen keyboard emits these from the d-pad WHEN IT IS HIDDEN, so an
 * application cannot tell a controller from a host keyboard — which is the
 * entire point of the input abstraction. */
#define SNOS_KEY_UP 0x1C
#define SNOS_KEY_DOWN 0x1D
#define SNOS_KEY_LEFT 0x1E
#define SNOS_KEY_RIGHT 0x1F

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

static uint16_t sys_net_image(const char *url) {
  snos_set_ptr(url);
  return sys_net_image_raw();
}

static uint16_t sys_read_f(void *buf, uint16_t max) {
  snos_set_ptr((const char *)buf);
  return sys_read_f_raw(max);
}

static uint16_t sys_net_read(uint16_t ch, void *buf, uint16_t max) {
  snos_set_ptr((const char *)buf);
  SNOS_ARGS.p2 = ch;
  return sys_net_read_raw(max);
}

/* Copy the launch argument out to a local buffer. Returns its length. */
static uint16_t snos_getargs(char *buf, uint16_t max) {
  volatile __far char *src = (volatile __far char *)SNOS_ARGV_ADDR;
  uint16_t i = 0;
  while (i + 1 < max && src[i]) {
    buf[i] = (char)src[i];
    i++;
  }
  buf[i] = 0;
  return i;
}

static uint16_t sys_puts_at(int row, int col, const char *s) {
  snos_set_ptr(s);
  return sys_puts_at_raw((uint16_t)(((uint16_t)row << 8) | (uint8_t)col));
}

#endif
