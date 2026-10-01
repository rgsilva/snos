/* Trap handlers, entered from the asm shims in calypsi/header.s.
 *
 * COP, BRK and NMI all force PB=$00 before the vector fetch, so their *entry
 * points* must live in the bank-$00 mirror. These C bodies are reached by JSL
 * and can live anywhere.
 *
 * ------------------------------------------------------------- the frame
 *
 * Both trap shims push the same thing in the same order, so one set of offsets
 * describes both. They hand this function the stack pointer as it stood after
 * those pushes; everything is read relative to that. The stack lives in bank
 * $00, which is the WRAM mirror of $7E0000, so the frame is reachable with
 * ordinary long addressing and no DB games.
 *
 *   sp+1  DB      sp+2  D      sp+4  Y      sp+6  X      sp+8  A
 *   sp+10 P       sp+11 PC     sp+13 PB
 *
 * The 65816 pushes PC already advanced past the COP signature byte, so the
 * signature is the byte at PB:PC-1. That is what makes `COP #n` a one-
 * instruction system call: the number is baked into the instruction stream and
 * the handler reads it back out.
 */
#include "snos.h"
#include "snos_abi.h"
#include "console.h"
#include "input.h"
#include "mailbox.h"
#include "farmem.h"
#include "fs.h"
#include "net.h"
#include "proc.h"

#define BANK0M 0x7E0000ul

/* kernel/netasm.s: n bytes between any two banks. */
extern void far_copy(uint8_t *dst, const uint8_t __far *src, uint16_t n);

#define FRM_DB 1
#define FRM_D 2
#define FRM_Y 4
#define FRM_X 6
#define FRM_A 8
#define FRM_P 10
#define FRM_PC 11
#define FRM_PB 13

volatile uint16_t snos_frames;

/* Set when a break arrives with no application running, so a long shell
 * operation (a download, say) can abort itself. Cleared by whoever acts on it. */
volatile uint16_t snos_break;

static uint8_t frm8(uint16_t sp, uint16_t off) {
  return *(volatile __far uint8_t *)(BANK0M + (uint32_t)(uint16_t)(sp + off));
}

static uint16_t frm16(uint16_t sp, uint16_t off) {
  return (uint16_t)(frm8(sp, off) | ((uint16_t)frm8(sp, off + 1) << 8));
}

/* Vblank, 60 Hz, kernel-owned unconditionally (PLAN.md §5). Runs on whatever
 * stack is current — while an app runs that is the APP's stack — so this must
 * stay shallow. No printf, no deep call chains, ever. */
static uint16_t kill_held;

void snos_nmi_c(void) {
  snos_frames++;
  /* BEFORE net_service, and load-bearing: pad_latch's first spin waits for
   * the auto-joypad read to START (~H32.5 of the first vblank line). Enter it
   * late — after a link poll — and the read may already be FINISHED, which
   * that spin cannot tell from "not started": it then burns its whole 2000-
   * iteration guard, tens of thousands of cycles, every frame. Tried the
   * other order on 2026-07-31 to save the ~700-cycle wait; the input suite
   * caught it within minutes. The wait is the cheap case. */
  pad_latch();

  /* Move one network frame. This is the reason the kernel owns NMI outright:
   * the link keeps running while an application has the machine, so a program
   * that never yields still gets its download.
   *
   * The display gets first claim on the vblank, though. NMI fires at the top of
   * the window and con_flush() does its DMA from the FOREGROUND inside it, so
   * an NMI that runs long enough consumes the window and the DMA lands during
   * active display, where VRAM writes are ignored. That failure is invisible in
   * every assertion — the mailbox shows perfect output while the screen sits
   * frozen — and a link poll costs more cycles than a vblank has.
   *
   * Deferring is self-balancing rather than a starvation risk: the console can
   * only stay dirty while something is printing, and something can only keep
   * printing while data keeps arriving, which requires this to run. The two
   * alternate. */
  if (!con_dirty_pending()) net_service();

  /* The kill switch. It lives here, in the interrupt the kernel owns
   * unconditionally, precisely so an application cannot defeat it: a wedged
   * app never has to call anything for this to fire. The shim in header.s
   * checks the flag and unwinds to app_return the moment we set it. */
  /* Keyboard break (F12). The controller chord below is unusable when the
   * controller is unplugged, which is how Ricardo actually runs the emulator —
   * so a keyboard-only escape hatch is not a convenience, it is the only one he
   * has. Consumed here whether or not an app is running: if one is, it dies; if
   * not, the flag is left for the foreground so a long-running shell command
   * can notice it too. */
  if (MB_BREAK) {
    MB_BREAK = 0;
    if (SNOS_EXEC_RUN) {
      SNOS_EXEC_CODE = SNOS_EXIT_KILLED;
      SNOS_EXEC_FLAG = 1;
    } else {
      snos_break = 1;
    }
  }

  if (SNOS_EXEC_RUN) {
    if ((pad_held() & (PAD_START | PAD_SELECT)) == (PAD_START | PAD_SELECT)) {
      if (++kill_held >= SNOS_KILL_FRAMES) {
        SNOS_EXEC_CODE = SNOS_EXIT_KILLED;
        SNOS_EXEC_FLAG = 1;
      }
    } else {
      kill_held = 0;
    }
  } else {
    kill_held = 0;
  }

  /* M10: file the previous context switch, decide the next one. Runs even
   * while a foreground death is armed — proc_tick then STEERS to process 0,
   * the only context the shims allow to take the unwind. The shim performs
   * the actual swap on its way out (header.s). */
  proc_tick();
}

/* ----------------------------------------------------------- COP dispatch */

static char rl_buf[128];

uint16_t snos_cop_c(uint16_t sp) {
  uint16_t pc = frm16(sp, FRM_PC);
  uint16_t pb = frm8(sp, FRM_PB);
  uint8_t sig = far_rd(pb, (uint16_t)(pc - 1));
  uint16_t a = frm16(sp, FRM_A);
  /* M10: who is calling, and do they hold the console? A background process
   * runs MUTED — console, input and filesystem-handle syscalls answer as
   * no-ops or errors unless the caller is the foreground process. The screen
   * and the keyboard have exactly one owner at a time; that was implicitly
   * true with one process and is enforced here now that there can be two.
   * (Direct PPU pokes cannot be gated — graphics apps are not
   * backgroundable, and their exclusive arena already refuses the pairing.)
   * The channel-owner tag is what net_reap uses so one app's exit cannot
   * tear down another's connections: 1 = the foreground app, 2.. = process
   * id + 1, 0 = the shell (never reaped). */
  uint8_t self = proc_current();
  int is_fg = (self == proc_fg());
  uint8_t owner_tag =
      (self == 0) ? (SNOS_EXEC_RUN ? 1u : 0u) : (uint8_t)(self + 1u);

  switch (sig) {
  case SYS_EXIT:
    /* A background process cannot unwind — there is no caller to unwind to.
     * Its channels are reaped here, its frame is rewritten to the grave
     * loop, and the scheduler never runs it again (proc.c). */
    if (self != 0) {
      net_reap(owner_tag);
      proc_exit_current(sp, a);
      return a;
    }
    /* Only arm the unwind if an app is actually running. A stray COP #0 from
     * kernel code would otherwise jump to app_return with a stale saved stack
     * pointer and take the machine down. */
    if (SNOS_EXEC_RUN) {
      SNOS_EXEC_CODE = a;
      SNOS_EXEC_FLAG = 1;
    }
    return a;

  case SYS_VERSION:
    return SNOS_ABI_VERSION;

  case SYS_YIELD:
    kbd_idle();
    return 0;

  case SYS_NOTIFY:
    /* Only meaningful from the background: the foreground already has the
     * screen. The kernel owns the cell, so no foreground app can be tricked
     * into erasing a pending notification. */
    if (!is_fg) proc_notify(self);
    return 0;

  case SYS_PUTC:
    if (!is_fg) return 0;
    kcon_putc((int)(a & 0xFFu));
    return 0;

  case SYS_PUTS: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint8_t c;
    if (!is_fg) return 0;
    while ((c = far_rd(bank, off)) != 0) {
      kcon_putc(c);
      off++;
    }
    return 0;
  }

  case SYS_GETC:
    /* Input belongs to the foreground. A background caller gets a NUL after
     * one frame of idling, so a polling loop does not spin flat out. */
    if (!is_fg) {
      kbd_idle();
      return 0;
    }
    return (uint16_t)kbd_getchar();

  case SYS_CLS:
    if (!is_fg) return 0;
    con_clear();
    return 0;

  case SYS_READLINE: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint16_t max = a, i;
    int n;
    if (!is_fg) {
      kbd_idle(); /* same pacing as background GETC: no flat-out spinning */
      if (max) far_wr(bank, off, 0);
      return 0;
    }
    if (max == 0) return 0; /* the copy below writes n+1 bytes: even an empty
                             * line lands the terminating NUL in app space */
    if (max > sizeof rl_buf) max = sizeof rl_buf;
    n = kbd_readline(rl_buf, (int)max);
    for (i = 0; i <= (uint16_t)n; i++) far_wr(bank, (uint16_t)(off + i), (uint8_t)rl_buf[i]);
    return (uint16_t)n;
  }

  case SYS_PUTU:
    if (!is_fg) return 0;
    kcon_putu(a);
    return 0;

  case SYS_PUTHEX:
    if (!is_fg) return 0;
    kcon_puthex16(a);
    return 0;

  /* ---- ABI 2: what a full-screen application needs ---- */

  /* Absolute positioning, clipped to the row. Deliberately goes through
   * con_putat rather than the cursor: it cannot scroll, and a full-screen
   * layout that can be scrolled out from under itself is not a layout. */
  case SYS_PUTS_AT: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    int r = (int)(a >> 8), c = (int)(a & 0xFFu);
    uint8_t ch;
    if (!is_fg) return 0;
    while (c < con_cols() && (ch = far_rd(bank, off)) != 0) {
      con_putat(r, c, ch);
      c++;
      off++;
    }
    return 0;
  }

  case SYS_SCREEN:
    return (uint16_t)(((uint16_t)con_text_rows() << 8) | (uint8_t)con_cols());

  case SYS_POLLKEY: {
    int c;
    if (!is_fg) return SYS_ERR; /* input belongs to the foreground */
    c = kbd_getchar_nonblock();
    return c < 0 ? SYS_ERR : (uint16_t)c;
  }

  /* The keyboard owns the d-pad while it is up, so an app that wants to
   * navigate a menu has to be able to put it away. */
  case SYS_OSK:
    /* The panel rides the foreground's screen and re-enables the HDMA
     * split; a background caller may only ask, never act. */
    if (!is_fg) return (uint16_t)(osk_visible() ? 1 : 0);
    if (a == 0) osk_hide();
    else if (a == 1) osk_show();
    return (uint16_t)(osk_visible() ? 1 : 0);

  case SYS_PAD:
    return is_fg ? pad_held() : 0;

  case SYS_OPEN: {
    char nm[FS_NAME_LEN + 1];
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    int i;
    /* Per-process handles since M10: background fs access is real (httpd
     * serves from the background), with one system-wide WRITE handle
     * enforced down in fs.c — the bump allocator has one commit cursor. */
    for (i = 0; i < FS_NAME_LEN; i++) {
      nm[i] = (char)far_rd(bank, (uint16_t)(off + i));
      if (nm[i] == 0) break;
    }
    nm[i < FS_NAME_LEN ? i : FS_NAME_LEN] = 0;
    if (a == 0) return fs_open_read(nm) == 0 ? 0 : SYS_ERR;
    return fs_open_write(nm) == 0 ? 0 : SYS_ERR;
  }

  case SYS_GETC_F: {
    int c = fs_getc();
    return c < 0 ? SYS_ERR : (uint16_t)c;
  }

  case SYS_PUTC_F:
    return fs_putc((uint8_t)a) == 0 ? 0 : SYS_ERR;

  case SYS_CLOSE:
    fs_close();
    return 0;

  case SYS_FSIZE:
    return (uint16_t)fs_open_size();

  case SYS_DIR: {
    const fs_entry_t *e = fs_get((int)a);
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    int i;
    if (e == 0) return 0;
    for (i = 0; i <= FS_NAME_LEN; i++)
      far_wr(bank, (uint16_t)(off + i), (uint8_t)e->name[i]);
    return 1;
  }

  case SYS_UNLINK: {
    char nm[FS_NAME_LEN + 1];
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    int i;
    for (i = 0; i < FS_NAME_LEN; i++) {
      nm[i] = (char)far_rd(bank, (uint16_t)(off + i));
      if (nm[i] == 0) break;
    }
    nm[i < FS_NAME_LEN ? i : FS_NAME_LEN] = 0;
    return fs_delete(nm) == 0 ? 0 : SYS_ERR;
  }

  case SYS_READ_F: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint16_t n = 0;
    int c;
    /* One trap for the whole block. SYS_GETC_F is a trap per byte, which is
     * fine for a BASIC listing and takes seconds for a 43 KB image. */
    while (n < a) {
      c = fs_getc();
      if (c < 0) break;
      far_wr(bank, (uint16_t)(off + n), (uint8_t)c);
      n++;
    }
    return n;
  }

  case SYS_READ_VRAM:
    if (!is_fg) return 0; /* VRAM is the display: foreground only */
    return fs_read_vram(a);

  case SYS_KBDPUSH:
    /* Not foreground-gated, deliberately: this is how a background telnetd
     * feeds the shell everyone can see. */
    return kbd_push((int)(a & 0xFFu)) == 0 ? 0 : SYS_ERR;

  case SYS_CONROW: {
    /* Deliberately NOT foreground-gated: mirroring the screen from the
     * background is telnetd's entire job. Read-only, so the single-owner
     * rules the other console syscalls enforce do not apply. */
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint8_t rowbuf[64];
    int i;
    if (a >= 28u) return SYS_ERR;
    if (con_gfx_owned()) return SYS_ERR; /* a picture owns the PPU */
    con_row_save((int)a, rowbuf);
    for (i = 0; i < 64; i++) far_wr(bank, (uint16_t)(off + i), rowbuf[i]);
    return 0;
  }

  case SYS_CONDIRTY: {
    /* Same standing as SYS_CONROW: background-allowed, because the mirror is
     * a background process by design. Read-and-clear is safe here — COP
     * handlers are never preempted (M10 switches only outside syscalls). */
    uint16_t lo, hi;
    if (con_gfx_owned()) return SYS_ERR;
    con_changes(&lo, &hi);
    SNOS_ARGS.p0 = lo;
    SNOS_ARGS.p1 = hi;
    return 0;
  }

  case SYS_GFX:
    if (!is_fg) return SYS_ERR; /* the PPU has one owner; see snos_cop_c top */
    if (a) {
      /* Hiding the keyboard is not cosmetic: it disables the HDMA split, which
       * rewrites the BG tilemap-base and scroll registers every frame and would
       * tear a Mode 3 picture apart. */
      osk_hide();
      osk_lock(1);
      con_gfx(1);
    } else {
      osk_lock(0);
      con_gfx(0);
    }
    return 0;

  case SYS_FRAMES:
    return snos_frames;

  case SYS_WAITVBL: {
    uint16_t f = snos_frames;
    while (snos_frames == f) {
    }
    return 0;
  }

  case SYS_NET_UP:
    return (uint16_t)(net_up() ? 1 : 0);

  /* $61 (SYS_NET_FETCH) retired at the uIP cutover: fetching is an HTTP
   * request the application composes itself over SYS_NET_CONNECT. It falls
   * through to the unknown-syscall path like any number that was never real. */

  case SYS_NET_STATS:
    SNOS_ARGS.p0 = net_stat_resent;
    SNOS_ARGS.p1 = net_stat_bad_sum;
    return net_stat_rx_frames;

  case SYS_NET_CONNECT: {
    char hp[256];
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    int i, r;
    for (i = 0; i < 255; i++) {
      hp[i] = (char)far_rd(bank, (uint16_t)(off + i));
      if (hp[i] == 0) break;
    }
    hp[i < 255 ? i : 255] = 0;
    r = net_connect(hp);
    if (r >= 0 && owner_tag) net_own(r, owner_tag);
    return r < 0 ? SYS_ERR : (uint16_t)r;
  }

  case SYS_NET_WRITE: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1, ch = SNOS_ARGS.p2;
    const uint8_t __far *src =
        (const uint8_t __far *)(((uint32_t)bank << 16) | (uint32_t)off);
    /* Copy through a local buffer: net_write takes a near pointer and the
     * source is in the application's bank. far_copy (netasm.s), because the
     * C loop here was ~840 master cycles a byte. */
    uint8_t tmp[128];
    uint16_t n = a;
    if (n > sizeof tmp) n = sizeof tmp;
    far_copy(tmp, src, n);
    return (uint16_t)net_write((int)ch, tmp, (int)n);
  }

  case SYS_NET_WSPACE:
    return (uint16_t)net_wspace((int)a);

  case SYS_NET_HANGUP:
    return net_hangup((int)a) < 0 ? SYS_ERR : 0;

  /* $6C/$6D (SYS_WEB_*) retired with the web app; they fall through to the
   * unknown-syscall path below like any other number that was never real. */

  case SYS_NET_LISTEN: {
    int r = net_listen(a);
    if (r >= 0 && owner_tag) net_own(r, owner_tag);
    return r < 0 ? SYS_ERR : (uint16_t)r;
  }

  case SYS_NET_READ: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1, ch = SNOS_ARGS.p2;
    uint8_t __far *dst = (uint8_t __far *)(((uint32_t)bank << 16) | (uint32_t)off);
    return (uint16_t)net_read((int)ch, dst, (int)a);
  }

  case SYS_FS_FREE:
    return (uint16_t)(fs_free() >> 10);

  case SYS_NET_STATE:
    /* p0/p1 (HTTP status, image flag) died with the gateway protocol: the
     * status line is bytes in the stream now, parsed by whoever asked for
     * it. The state alone is the answer. */
    return (uint16_t)net_state((int)a);

  case SYS_NET_CLOSE:
    net_close((int)a);
    return 0;

  case SYS_TRACE: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint8_t c;
    while ((c = far_rd(bank, off)) != 0) {
      mb_putc((char)c);
      off++;
    }
    return 0;
  }

  default:
    return SYS_ERR;
  }
}

/* --------------------------------------------------------------- BRK ---- */

static void dump(const char *name, uint16_t v) {
  kcon_puts(name);
  kcon_puthex16(v);
  kcon_putc(' ');
}

/* Debugger trap. Never returns. Prints a register dump on screen AND into the
 * mailbox, so a panic is legible both to a human watching the TV and to the
 * automated harness. */
void snos_brk_c(uint16_t sp) {
  kcon_puts("\n*** BRK ***\n");
  dump("PB:", frm8(sp, FRM_PB));
  dump("PC:", frm16(sp, FRM_PC));
  dump("P:", frm8(sp, FRM_P));
  kcon_putc('\n');
  dump("A:", frm16(sp, FRM_A));
  dump("X:", frm16(sp, FRM_X));
  dump("Y:", frm16(sp, FRM_Y));
  kcon_putc('\n');
  dump("D:", frm16(sp, FRM_D));
  dump("DB:", frm8(sp, FRM_DB));
  dump("S:", sp);
  kcon_putc('\n');
  kcon_sync();

  mb_puts("\n[BRK]\n");
  mb_finish(MB_STATUS_PANIC);
}
