/* SNOS telnetd — remote into a Super Nintendo.
 *
 *     telnetd [PORT]        default 2323
 *
 * Run it in the background (`telnetd &`) and `telnet <the address net status
 * prints> 2323` from
 * anywhere: you get the console's screen, live, and your keystrokes go into
 * the same input queue the on-screen keyboard uses. Both ends see everything
 * — remote and real console mirror each other — because this is a MIRROR,
 * not a session. There is no multi-user anything; that is the point.
 *
 * ------------------------------------------------------ why diffing, not tee
 *
 * The obvious build is to tee console writes into a byte stream. It fails on
 * the first `clear`, because a stream carries characters and a screen is
 * STATE: clears, positioned writes (how edit and irc paint everything) and
 * scrolls are operations on state that a character stream cannot express.
 * Recording operations instead would need an event ring, which one irc
 * repaint overflows.
 *
 * So: read the console's own shadow a row at a time (SYS_CONROW) and send
 * only what CHANGED. Overflow is impossible — the screen is 1792 bytes and
 * we hold a copy of it — bursts coalesce for free (ten repaints between two
 * polls cost one diff), and clear/putat/scroll all fall out uniformly
 * because they are just... different bytes in the shadow. The console never
 * learns that a terminal exists; it narrates its state and the far end draws.
 *
 * SYS_CONDIRTY narrows the scan. The kernel tracks which rows changed for
 * its own VRAM flush, so asking it beats re-reading 28 rows to discover 27
 * did not change: a keystroke's echo is one row read instead of 28, an idle
 * screen is none — cheap enough to poll every frame, which is what turns
 * "up to 100 ms before the diff even starts" into "next frame". A scroll
 * reports every row, because every row's content genuinely moved; the
 * mirror diff still keeps the wire cost to the rows that differ.
 *
 * -------------------------------------------------------------------- ANSI
 *
 * Every telnet client is a VT100 underneath. A changed row is
 * `ESC[<row>;1H`, the text, `ESC[K`; inverse-video characters (the shadow's
 * $80-$FF twins — the shell's caret, the OSK's highlight, irc's tab bar) are
 * wrapped in `ESC[7m`/`ESC[0m`. That is the entire renderer.
 *
 * The SNES emulates no terminal and parses no escape sequences: what comes
 * back is stripped of telnet negotiation, mapped to ASCII, and pushed into
 * the input queue one byte at a time.
 */
#include "snos_abi.h"
#include "snoslib.h"

#define ROWS 28
#define COLS 64
/* Every frame. This was 6 (~10 Hz) when a pass meant reading all 28 rows;
 * with SYS_CONDIRTY a pass on a quiet screen is one trap returning an empty
 * mask, so there is nothing left to ration. */
#define PAINT_EVERY 1u

/* Telnet: IAC WILL ECHO + IAC WILL SUPPRESS-GO-AHEAD puts the client in
 * character-at-a-time mode. Without it, telnet line-buffers and the mirror
 * only updates when you press Enter — which would defeat the whole thing. */
static const uint8_t NEGOTIATE[] = {255, 251, 1, 255, 251, 3};

static char args[SNOS_ARGV_MAX];
static uint8_t mirror[ROWS][COLS]; /* what the far end is known to show */
static uint8_t row[COLS];
static uint8_t rx[128];
static char out[768];
static uint16_t outn;

static void flush_out(uint16_t ch) {
  if (outn == 0) return;
  sl_send_all(ch, out, outn);
  outn = 0;
}

static void emit(uint16_t ch, char c) {
  if (outn >= sizeof out) flush_out(ch);
  out[outn++] = c;
}

static void emit_str(uint16_t ch, const char *s) {
  while (*s) emit(ch, *s++);
}

static void emit_u(uint16_t ch, uint16_t v) {
  char d[6];
  int n = 0;
  if (v == 0) {
    emit(ch, '0');
    return;
  }
  while (v && n < 6) {
    d[n++] = (char)('0' + (v % 10u));
    v = (uint16_t)(v / 10u);
  }
  while (n) emit(ch, d[--n]);
}

/* ESC[<r>;<c>H — VT100 rows and columns are 1-based. */
static void go_to(uint16_t ch, uint16_t r, uint16_t c) {
  emit(ch, 27);
  emit(ch, '[');
  emit_u(ch, (uint16_t)(r + 1u));
  emit(ch, ';');
  emit_u(ch, (uint16_t)(c + 1u));
  emit(ch, 'H');
}

static void sgr(uint16_t ch, const char *code) {
  emit(ch, 27);
  emit(ch, '[');
  emit_str(ch, code);
  emit(ch, 'm');
}

/* One changed row, positioned and cleared to the end so trailing spaces cost
 * three bytes instead of sixty. */
static void paint_row(uint16_t ch, uint16_t r, const uint8_t *src) {
  int last = COLS - 1, i, inv = 0;
  while (last >= 0 && (src[last] & 0x7F) == ' ') last--;
  go_to(ch, r, 0);
  for (i = 0; i <= last; i++) {
    uint8_t c = src[i];
    int want = (c & 0x80) ? 1 : 0;
    if (want != inv) {
      sgr(ch, want ? "7" : "0");
      inv = want;
    }
    c &= 0x7F;
    emit(ch, (char)((c >= 0x20 && c <= 0x7E) ? c : ' '));
  }
  if (inv) sgr(ch, "0");
  emit(ch, 27);
  emit(ch, '[');
  emit(ch, 'K');
}

int main(void) {
  uint16_t port, ch, st;
  uint16_t connected = 0, next_paint = 0, gfx_told = 0, force_all = 0;
  int i, j;

  snos_getargs(args, sizeof args);
  port = sl_parse_port(args, 2323);

  if (!sys_net_up()) {
    sys_puts("no link attached\n");
    return 1;
  }
  ch = sys_net_listen(port);
  if (ch == SYS_ERR) {
    sys_puts("cannot listen (no free channel, or port in use)\n");
    return 1;
  }
  sys_puts("telnetd on port ");
  sys_putu(port);
  sys_puts(" - 'net status' shows the address\n");
  sys_puts("run it with & to keep using this console\n");

  for (;;) {
    st = sys_net_state(ch);

    if (st == NET_ST_OPEN || st == NET_ST_EOF) {
      if (!connected) {
        connected = 1;
        gfx_told = 0;
        sys_notify(); /* somebody just remoted in; the cell says so */
        for (i = 0; i < (int)sizeof NEGOTIATE; i++)
          emit(ch, (char)NEGOTIATE[i]);
        /* Clear, and hide the terminal's own cursor: the console's caret is
         * already in the shadow as an inverse character, so a second blinking
         * block would just be a liar in a different place. */
        emit_str(ch, "\033[2J\033[?25l");
        /* Force a full repaint by making the known state impossible. The
         * kernel's change mask cannot help here — it says what changed since
         * the LAST query, and this client has seen nothing at all. */
        for (i = 0; i < ROWS; i++)
          for (j = 0; j < COLS; j++) mirror[i][j] = 0xFF;
        force_all = 1;
        next_paint = sys_frames();
      }

      /* ---- screen out ---- */
      if ((int16_t)(sys_frames() - next_paint) >= 0) {
        next_paint = (uint16_t)(sys_frames() + PAINT_EVERY);
        if (sys_condirty() == SYS_ERR) {
          /* An application owns the PPU: there is no text to mirror, and a
           * Mode 3 picture is not going down a telnet pipe. Say so once, and
           * forget the whole mirror — the far screen no longer shows it. */
          if (!gfx_told) {
            gfx_told = 1;
            emit_str(ch, "\033[2J\033[1;1H[graphics on the real screen]");
            for (i = 0; i < ROWS; i++)
              for (j = 0; j < COLS; j++) mirror[i][j] = 0xFF;
            flush_out(ch);
          }
        } else {
          uint16_t mlo = SNOS_ARGS.p0, mhi = SNOS_ARGS.p1, bit;
          if (gfx_told || force_all) {
            /* The far screen holds who-knows-what (the graphics banner, or a
             * fresh client's nothing): check every row against the mirror.
             * The query above still ran, so the kernel's mask is drained. */
            mlo = 0xFFFFu;
            mhi = 0x0FFFu;
            gfx_told = 0;
            force_all = 0;
          }
          if (mlo | mhi) {
            uint16_t nrows = (uint16_t)(sys_screen() >> 8);
            if (nrows > ROWS) nrows = ROWS;
            bit = 1;
            for (i = 0; i < (int)nrows; i++) {
              uint16_t hit = (i < 16) ? (uint16_t)(mlo & bit)
                                      : (uint16_t)(mhi & bit);
              bit = (uint16_t)(i == 15 ? 1u : bit << 1);
              if (!hit) continue;
              if (sys_conrow((uint16_t)i, row) == SYS_ERR) break;
              for (j = 0; j < COLS; j++)
                if (mirror[i][j] != row[j]) break;
              if (j == COLS) continue; /* changed back, or repainted alike */
              paint_row(ch, (uint16_t)i, row);
              for (j = 0; j < COLS; j++) mirror[i][j] = row[j];
            }
            flush_out(ch);
          }
        }
      }

      /* ---- keys in ---- */
      {
        uint16_t n = sys_net_read(ch, rx, sizeof rx);
        if (n > 0 && n != SYS_ERR) {
          for (i = 0; i < (int)n; i++) {
            uint8_t c = rx[i];
            if (c == 255) { /* IAC: skip the whole three-byte command */
              i += 2;
              continue;
            }
            if (c == '\r') {
              sys_kbdpush('\n');
              /* CR NUL and CR LF both mean one Enter. */
              if (i + 1 < (int)n && (rx[i + 1] == 0 || rx[i + 1] == '\n')) i++;
              continue;
            }
            if (c == '\n') {
              sys_kbdpush('\n');
              continue;
            }
            if (c == 0x7F || c == 8) {
              sys_kbdpush('\b');
              continue;
            }
            /* Control characters pass through: the apps want ^N, ^Q, ^U. */
            if (c == 0 || c > 0x7E) continue;
            sys_kbdpush(c);
          }
        }
      }

      if (st == NET_ST_EOF) {
        /* The client hung up. Bounded like every other server here: a peer
         * that stops draining must not pin a background process forever. */
        uint16_t give_up = (uint16_t)(sys_frames() + 300u);
        outn = 0;
        connected = 0;
        for (;;) {
          if (sys_net_hangup(ch) != SYS_ERR) break;
          if ((int16_t)(sys_frames() - give_up) >= 0) {
            sys_net_close(ch);
            ch = sys_net_listen(port);
            if (ch == SYS_ERR) return 1;
            break;
          }
          if (sys_pollkey() != SYS_ERR) return 0;
          sys_yield();
        }
      }
    } else if (st == NET_ST_ERROR) {
      sys_puts(connected ? "telnetd: connection error\n"
                         : "telnetd: listen failed - port already in use?\n");
      break;
    }

    if (sys_pollkey() != SYS_ERR) break;
    sys_yield();
  }

  sys_net_close(ch);
  sys_puts("telnetd stopped\n");
  return 0;
}
