/* SNOS input layer: controller, on-screen keyboard, line editor.
 *
 * The keyboard is a panel across the bottom of the screen, drawn as text in the
 * console's own font — so it is exactly as crisp as the terminal, 8 pixels per
 * glyph in Mode 5's 512-dot space.
 *
 * It is NOT drawn into the text tilemaps. Those scroll, and an earlier version
 * that lived in them drifted up through the display and corrupted whatever it
 * landed on. It has its own pair of tilemaps held still by an HDMA split
 * (console.c): text scrolls above the split, the panel below it never moves,
 * and the console's text area shrinks so nothing scrolls underneath.
 *
 * A sprite version fixed the scrolling too, but sprites are locked to 256 dots
 * in Mode 5 while the background is 512 — so every glyph came out twice as wide
 * as the surrounding text. That is hardware, not styling, and it looked awful.
 */
#include "input.h"
#include "console.h"
#include "mailbox.h"
#include "snos.h"
#include "proc.h"
#include "ip.h"
#include "net.h"

#define R8(a) (*(volatile uint8_t *)(a))

/* ------------------------------------------------------------------ pad */

static volatile uint16_t pad_cur, pad_edge;

/* Called from the NMI handler.
 *
 * The subtle part: NMI fires at the TOP of vblank, but the auto-joypad read
 * does not begin until H=32.5..95.5 of that first vblank scanline and then
 * takes 4224 master cycles. So on entry $4212 bit 0 is typically still CLEAR,
 * and a naive "wait while busy" falls straight through and samples $4218
 * mid-shift. Wait for the read to START and then to FINISH.
 *
 * Both waits are bounded: if auto-joypad read is ever disabled, bit 0 never
 * sets, and an unbounded spin here would hang the machine inside an NMI. */
void pad_latch(void) {
  uint16_t v, prev, guard = 0;
  while (!(R8(0x4212) & 0x01) && ++guard < 2000u) {
  }
  while ((R8(0x4212) & 0x01) && ++guard < 4000u) {
  }
  v = (uint16_t)(R8(0x4218) | (R8(0x4219) << 8));
  MB_FLIPS = v; /* debug: raw pad word, so the harness can see the wire */
  prev = pad_cur;
  pad_cur = v;
  pad_edge |= (uint16_t)(v & ~prev);
}

/* The controller, and the adapter's keyboard as one (LINK_CH_KBDPAD, whose
 * words net.c keeps as they arrive). The keyboard's never reach pad_cur and
 * pad_edge, which are also the on-screen keyboard's: SELECT opens the panel
 * and the d-pad walks it, and a game played from the keyboard must not open
 * the panel on Right Shift. */
uint16_t pad_held(void) { return (uint16_t)(pad_cur | net_kbdpad); }

uint16_t pad_phys(void) { return pad_cur; }

/* Read-and-clear has to be atomic against the NMI that ORs into pad_edge, or a
 * button press landing in the window is silently dropped. NMI is not maskable
 * with SEI, so mask it at the source ($4200 bit 7) instead. */
uint16_t pad_pressed(void) {
  uint16_t e;
  R8(0x4200) = 0x01; /* NMI off, auto-joypad still on */
  e = pad_edge;
  pad_edge = 0;
  R8(0x4200) = 0x81;
  return e;
}

/* The controller is sampled once a frame, so a press shorter than that never
 * existed for it; the keyboard's arrive as records, and a press and its
 * release can land between two of an app's reads -- queued behind a
 * datagram, two records in one frame. So a keyboard press shows here once
 * even if it is already over, if it is recent (NET_KBDPAD_TAP): nothing
 * reads SYS_PAD at the shell, and what was pressed there is not news to the
 * next app. Masked like pad_pressed, for the same reason. */
uint16_t pad_read(void) {
  uint16_t v;
  R8(0x4200) = 0x01;
  v = pad_held();
  if ((uint16_t)(snos_frames - net_kbdpad_rose_at) <= NET_KBDPAD_TAP) v |= net_kbdpad_rose;
  net_kbdpad_rose = 0;
  R8(0x4200) = 0x81;
  return v;
}

/* ------------------------------------------------------------------ OSK */

/* Six rows across the bottom: a framed panel with the key grid on the left and
 * the controller mapping on the right, so the controls are visible exactly when
 * you need them rather than buried in `help`.
 *
 * At 32 columns the grid alone fills the width, so the mapping moves into the
 * frame's top and bottom lines instead. Still six rows either way, so an
 * application sees the same 22-row text area in both modes. */
#define PANEL_ROWS 6
#define PANEL_TOP (28 - PANEL_ROWS)
#define KEY_ROWS 4
#define KEY_COL0 3   /* left edge of the key grid */
#define KEY_COL0_32 4 /* ...at 32 columns: centred, three blank columns a side */
#define KEY_STEP 2   /* one blank column between keys, so it reads as a grid */
#define HINT_COL 34  /* controller mapping starts here */

static const char *const KEYS_LO[KEY_ROWS] = {
    "1234567890-=",
    "qwertyuiop[]",
    "asdfghjkl;'",
    "zxcvbnm,./",
};
static const char *const KEYS_HI[KEY_ROWS] = {
    "!@#$%^&*()_+",
    "QWERTYUIOP{}",
    "ASDFGHJKL:\"",
    "ZXCVBNM<>?",
};

/* One per key row, shown to the right of the grid. */
static const char *const HINTS[KEY_ROWS] = {
    "A type key      START  enter",
    "B backspace     SELECT hide",
    "X space",
    "Y shift",
};

/* The same mapping at 32 columns, set into the frame from column 2; each '|'
 * is drawn as a stretch of the frame line. */
static const char HINT_TOP_32[] = "A type|B bksp|X space|Y shift";
static const char HINT_BOT_32[] = "START enter|SELECT hide";

static uint8_t panel_buf[64];
/* Which rows still need drawing is con_panel_dirty, in console.c: the NMI
 * holds the link off while any is set, or the rows never get a vblank. */

/* Starts CLOSED. It costs six rows of screen and most sessions drive SNOS from
 * a host keyboard through the mailbox, so having it up by default taxes the
 * common case to serve the rare one. SELECT brings it up, and the boot message
 * says so. */
static int osk_on;
static int osk_shift;
static int sel_row = 1, sel_col;
static uint16_t repeat_at;

/* Set while an application owns the PPU. SELECT must not be able to bring the
 * keyboard back then: showing it re-enables the HDMA split, which rewrites the
 * BG tilemap-base and scroll registers every scanline and tears a Mode 3
 * picture apart underneath an app that has no idea it happened. */
static int osk_locked;

static int keyrow_len(int r) {
  const char *s = osk_shift ? KEYS_HI[r] : KEYS_LO[r];
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void clamp_sel(void) {
  int n = keyrow_len(sel_row);
  if (sel_col >= n) sel_col = n - 1;
  if (sel_col < 0) sel_col = 0;
}

/* Text inside the frame, never over its right edge `right`. A '|' in the
 * text is a piece of the frame line (the 32-column hints). */
static void put(uint8_t *b, int col, int right, const char *s) {
  for (; *s && col < right; s++)
    b[col++] = (uint8_t)(*s == '|' ? CON_BOX_H : *s);
}

/* Laid out for the width the console has NOW: osk_show after a mode switch
 * redraws every row, so a row is never built for one width and shown in
 * the other. */
static void build_row(uint8_t *b, int prow) {
  int i;
  int narrow = con_cols() == 32;
  int right = con_cols() - 1; /* the frame's right edge */

  for (i = 0; i < 64; i++) b[i] = ' ';

  /* Frame */
  if (prow == 0) {
    for (i = 1; i < right; i++) b[i] = CON_BOX_H;
    b[0] = CON_BOX_TL;
    b[right] = CON_BOX_TR;
    if (narrow) put(b, 2, right, HINT_TOP_32);
    return;
  }
  if (prow == PANEL_ROWS - 1) {
    for (i = 1; i < right; i++) b[i] = CON_BOX_H;
    b[0] = CON_BOX_BL;
    b[right] = CON_BOX_BR;
    if (narrow) put(b, 2, right, HINT_BOT_32);
    return;
  }
  b[0] = CON_BOX_V;
  b[right] = CON_BOX_V;

  {
    int krow = prow - 1;
    const char *s = osk_shift ? KEYS_HI[krow] : KEYS_LO[krow];
    int col0 = narrow ? KEY_COL0_32 : KEY_COL0;

    /* Stagger each row by one column, the way a real keyboard is offset. */
    for (i = 0; s[i]; i++) {
      int c = col0 + krow + i * KEY_STEP;
      if (c >= right - 1) break;
      /* $80 selects the inverse-video twin of the glyph — the selection
       * highlight, since Mode 5 has no spare layer to draw a cursor on. */
      b[c] = (uint8_t)(s[i] | ((krow == sel_row && i == sel_col) ? 0x80 : 0x00));
    }
    if (!narrow) put(b, HINT_COL, right, HINTS[krow]);
  }
}

void osk_lock(int on) { osk_locked = on; }

void osk_show(void) {
  osk_on = 1;
  con_panel_dirty = (1 << PANEL_ROWS) - 1;
  con_split_enable(PANEL_TOP);
}

void osk_hide(void) {
  osk_on = 0;
  con_panel_dirty = 0;
  con_split_disable();
}

int osk_visible(void) { return osk_on; }

/* One row per call, so a repaint never overruns a vblank. Only panel changes
 * cost anything now — scrolling text no longer disturbs it at all, which is
 * the entire point of the split. */
void osk_redraw(void) {
  int i;
  if (!osk_on || !con_panel_dirty) return;
  for (i = 0; i < PANEL_ROWS; i++) {
    if (con_panel_dirty & (1 << i)) {
      build_row(panel_buf, i);
      /* Cleared only when the DMA landed inside vblank; an overrun (the NMI
       * ran long) retries the row next call instead of leaving it
       * half-painted with the flag gone. While a bit is set the NMI leaves
       * the link alone, which is what lets the retry land. */
      if (con_panel_row(PANEL_TOP + i, panel_buf))
        con_panel_dirty &= (uint8_t)~(1 << i);
      return;
    }
  }
}

static void move_sel(int dr, int dc) {
  int old = sel_row;
  if (dr) {
    sel_row = (sel_row + dr + KEY_ROWS) % KEY_ROWS;
    clamp_sel();
    con_panel_dirty |= (uint8_t)((1 << (old + 1)) | (1 << (sel_row + 1)));
  }
  if (dc) {
    int n = keyrow_len(sel_row);
    sel_col = (sel_col + dc + n) % n;
    con_panel_dirty |= (uint8_t)(1 << (sel_row + 1));
  }
}

int osk_poll(void) {
  uint16_t p = pad_pressed();
  uint16_t h = pad_cur; /* the controller's, never the keyboard's: pad_held */

  /* Auto-repeat: without it, crossing the keyboard one press at a time is
   * miserable. Hold for ~1/3 s, then repeat every 4 frames. */
  if (h & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
    if (repeat_at == 0) {
      repeat_at = (uint16_t)(snos_frames + 20u);
    } else if ((int16_t)(snos_frames - repeat_at) >= 0) {
      repeat_at = (uint16_t)(snos_frames + 4u);
      p |= (uint16_t)(h & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT));
    }
  } else {
    repeat_at = 0;
  }

  if (!p) return -1;

  if (p & PAD_SELECT) {
    if (osk_locked) return -1;
    if (osk_on) osk_hide();
    else osk_show();
    return -1;
  }
  if (p & PAD_START) return '\n';
  if (p & PAD_B) return '\b';
  if (p & PAD_X) return ' ';

  /* With the keyboard hidden the d-pad is nobody's, so hand it to the
   * application as arrow keys. Above this layer a controller and a host
   * keyboard then look identical, which is what lets the editor's menu be
   * driven by either without knowing which is present. */
  if (!osk_on) {
    if (p & PAD_UP) return SNOS_KEY_UP;
    if (p & PAD_DOWN) return SNOS_KEY_DOWN;
    if (p & PAD_LEFT) return SNOS_KEY_LEFT;
    if (p & PAD_RIGHT) return SNOS_KEY_RIGHT;
    return -1;
  }

  if (p & PAD_Y) {
    osk_shift = !osk_shift;
    clamp_sel();
    con_panel_dirty |= 0x1E; /* all four key rows */
    return -1;
  }
  if (p & PAD_UP) move_sel(-1, 0);
  if (p & PAD_DOWN) move_sel(1, 0);
  if (p & PAD_LEFT) move_sel(0, -1);
  if (p & PAD_RIGHT) move_sel(0, 1);

  if (p & PAD_A) {
    const char *s = osk_shift ? KEYS_HI[sel_row] : KEYS_LO[sel_row];
    return (int)(unsigned char)s[sel_col];
  }
  return -1;
}

/* ------------------------------------------------------- keyboard facade */

void kbd_init(void) {
  R8(0x4200) = 0x81; /* NMI + auto-joypad read */
}

/* Inject a character as if typed. The mailbox stdin ring is already the
 * input layer's third backend (the harness and kbdbridge type through it),
 * so a remote terminal is just a fourth producer into the same funnel --
 * which is what makes telnetd's local mirroring fall out for free. */
int kbd_push(int c) {
  uint16_t wi = MB_IN_WINDEX;
  if ((uint16_t)(wi - MB_IN_RINDEX) >= MB_IN_RING_SIZE) return -1;
  MB_IN_RING[wi % MB_IN_RING_SIZE] = (uint8_t)c;
  MB_IN_WINDEX = (uint16_t)(wi + 1);
  return 0;
}

int kbd_getchar_nonblock(void) {
  int c = mb_getc_nonblock(); /* the harness types here */
  if (c >= 0) return c;
  return osk_poll();
}

/* May a foreground link poll run on the CURRENT stack? The spin below runs
 * on whatever stack the blocking caller owns — a process slot's 1536 bytes,
 * more often than not — and those were sized against a high-water that did
 * not include a foreground net_service chain on top of the blocker. So the
 * poll pays its way only where there is headroom: current depth against the
 * containing stack's floor, with room for the whole poll chain AND the NMI
 * that lands on its residue at unmask. Shallow blockers — a shell at its
 * prompt, an app pumping a download — poll; anything deep just spins as it
 * always did and the NMI's 60 Hz poll carries the link. */
#define NET_FG_STACK_NEED 640u
static int fg_poll_room(void) {
  uint8_t probe;
  uint16_t sp = (uint16_t)(uint32_t)&probe;
  uint16_t floor;
  if (sp >= (uint16_t)SNOS_PSTACK_LO(1)) floor = (uint16_t)SNOS_PSTACK_LO(1);
  else if (sp >= (uint16_t)SNOS_PSTACK_LO(0)) floor = (uint16_t)SNOS_PSTACK_LO(0);
  else floor = SNOS_KSTACK_LO;
  return (uint16_t)(sp - floor) >= NET_FG_STACK_NEED;
}

/* May a poll START RIGHT NOW? A poll masks NMI for its whole span — ~14k
 * cycles at LINK_FRAME 256, seventy-odd scanlines — and the ONE thing it
 * must never do is still be masked when vblank begins. A delayed NMI does
 * not merely jitter: pad_latch's wait-for-start spin cannot tell "finished
 * long ago" from "not started", so a late NMI burns its full guard budget —
 * the better part of a frame INSIDE the handler — and the handler then
 * spans the NEXT vblank start, where the 65816 happily takes a nested NMI
 * (the I flag does not mask it). Each nested handler is also late, also
 * burns, and the cascade eats the stack down to the paint; it surfaced as a
 * scheduler resume from never-written stack, A5A5 in every register. The
 * $4212 vblank-flag check this replaces only refused to START in vblank —
 * it happily started a 70-line poll on line 220.
 *
 * So: latch the V counter and require the poll to FIT. Lines 0..POLL_LAST
 * leave 90+ lines before vblank at 225; lines 227+ are safe for the
 * opposite reason — this frame's NMI has already fired and the next is a
 * full frame away. Only the closing window [POLL_LAST+1, 226] is refused,
 * and the spin just re-asks a few lines later. The 9-bit V counter's high
 * bit is deliberately ignored: lines 256..261 alias to 0..5, which the
 * early zone allows — correctly, they are deep in the post-NMI region. */
#define POLL_LAST 134u
uint8_t beam_line(void) {
  uint8_t v;
  (void)R8(0x213F); /* reset the OPVCT read toggle to the low half */
  (void)R8(0x2137); /* latch H/V */
  v = R8(0x213D);   /* OPVCT low eight bits */
  (void)R8(0x213F); /* leave the toggle reset for the next caller */
  return v;
}

static int fg_poll_fits(void) {
  uint8_t v = beam_line();
#ifdef LINK_HW
  /* A cable poll is not a mailbox copy: the exchange alone is ~112k master
   * cycles (82 lines), and the C before it measures up to ~40 more -- far
   * from "most of the way to the next vblank", but not seventy lines either.
   * link_serial.c owns that arithmetic, the per-frame budget (`net tune
   * exch`) and a last check of its own just before the clocks start. */
  return link_serial_fg_fits(v);
#else
  return v <= POLL_LAST || v >= 227u;
#endif
}

void kbd_idle(void) {
  uint16_t f = snos_frames;
  /* Every blocking loop in the kernel funnels through here, so this is THE
   * in-syscall switch point: without it, a process blocked in SYS_GETC (the
   * shell at its prompt, most of the time) is mid-syscall at every NMI, the
   * deferral always says no, and background processes starve forever. */
  proc_switch_now();
  if (net_fg_wanted() && fg_poll_room()) {
    /* This spin used to be dead time, and it is most of every second. Spend
     * it polling the link — but only while a transfer is live (net_fg_wanted,
     * so an idle prompt does not spin the bridge), only with stack headroom
     * (fg_poll_room), and only when the poll FITS before the next vblank
     * (fg_poll_fits — the NMI must never be masked across line 225, see
     * above). The spin re-checks all three before every poll. */
    while (snos_frames == f) {
      if (net_fg_wanted() && fg_poll_fits()) net_service_fg();
    }
  } else {
    while (snos_frames == f) {
    }
  }
#ifdef LINK_HW
  /* The frame has just turned and the NMI has had its cable exchange, which
   * leaves the beam near line 90 -- the only place a ~100-line foreground
   * exchange can still fit before the next vblank. The spin above rarely
   * gets one: by the time the display and stack work below and the
   * application are done, the window has shut. So the foreground's extra
   * exchange is taken here, first, while the caller is waiting anyway; the
   * work below waits for it, and link_serial_fg_fits keeps the frame's
   * budget (docs/link-throughput.md). */
  if (net_fg_wanted() && fg_poll_room() && fg_poll_fits()) net_service_fg();
#endif
  /* The IP layer's foreground pump (M11). Here for the same reason the OSK
   * is serviced here: every blocking loop funnels through, so the stack gets
   * cycles whenever anything waits — which is most of every second. It
   * guards itself against the two processes arriving at once, and it is NOT
   * foreground-gated: a background server's packets matter exactly as much. */
  /* Display work belongs to the FOREGROUND process only. A background
   * process yielding through here used to run con_flush concurrently with
   * the shell's own printing — two writers in the console's row buffers and
   * DMA channels, which corrupted the display state within a frame of the
   * first background spawn. */
  if (proc_current() == proc_fg()) {
    osk_redraw();
    con_flush();
  }
  ip_service();
}

int kbd_getchar(void) {
  for (;;) {
    int c = kbd_getchar_nonblock();
    if (c >= 0) return c;
    /* A dying app must not block its own funeral. The NMI defers the kill
     * unwind while a syscall is on the stack (header.s), so a SYS_GETC that
     * kept waiting here would make the app unkillable; give up instead and
     * let the COP shim unwind on return. Foreground (shell) callers never
     * see this: APPRUNNING is only set while an app owns the machine. */
    if (SNOS_EXEC_RUN && SNOS_EXEC_FLAG) return -1;
    kbd_idle();
  }
}

/* ------------------------------------------------------------ line editor */

/* The console owns the caret: showing or hiding the keyboard scrolls the view
 * between drawing it and erasing it, and only the console knows where it
 * actually ended up. */
static void caret(int on) { con_caret(on); }

/* Command history.
 *
 * A ring of eight rather than the single entry asked for: it is a handful of
 * extra lines and the difference between "repeat that" and "where was that
 * command". Lives in far RAM — 1 KB has no business on a 2816-byte stack.
 *
 * Blank lines and exact repeats are not recorded. Holding a key down to walk
 * back through history otherwise fills the ring with copies of whatever you
 * were already looking at. */
#define HIST_N 8
#define HIST_LEN 128

static char hist[HIST_N][HIST_LEN];
static uint8_t hist_count; /* how many slots are populated, saturating at N */
static uint8_t hist_head;  /* where the next entry goes */

static void hist_add(const char *s) {
  int i;
  if (s[0] == 0) return;
  if (hist_count) {
    const char *prev = hist[(hist_head + HIST_N - 1) % HIST_N];
    for (i = 0;; i++) {
      if (prev[i] != s[i]) break;
      if (s[i] == 0) return; /* identical to the last entry */
    }
  }
  for (i = 0; i < HIST_LEN - 1 && s[i]; i++) hist[hist_head][i] = s[i];
  hist[hist_head][i] = 0;
  hist_head = (uint8_t)((hist_head + 1) % HIST_N);
  if (hist_count < HIST_N) hist_count++;
}

/* back = 1 is the most recent entry. Returns NULL past the end. */
static const char *hist_get(int back) {
  if (back < 1 || back > (int)hist_count) return 0;
  return hist[(hist_head + HIST_N - back) % HIST_N];
}

/* Wipe what is on screen and put `src` there instead. The cursor must be at
 * the end of the line. Backspace steps back across a wrapped row (con_putc),
 * so a line longer than the screen is erased whole. */
static int line_replace(char *buf, int n, const char *src, int max) {
  int i;
  while (n) {
    kcon_putc('\b');
    kcon_putc(' ');
    kcon_putc('\b');
    n--;
  }
  for (i = 0; src && src[i] && i < max - 1; i++) {
    buf[i] = src[i];
    kcon_putc(src[i]);
  }
  buf[i] = 0;
  return i;
}

/* Reprint buf[from..to) over itself, which moves the cursor right past it. */
static void line_show(const char *buf, int from, int to) {
  while (from < to) kcon_putc(buf[from++]);
}

static void line_back(int k) {
  while (k-- > 0) kcon_putc('\b');
}

/* Take buf[at] out: the tail moves left one, a space covers the cell it
 * vacated, and the cursor comes back to `at`. Returns the new length. */
static int line_delete(char *buf, int n, int at) {
  int i;
  for (i = at; i < n - 1; i++) buf[i] = buf[i + 1];
  n--;
  line_show(buf, at, n);
  kcon_putc(' ');
  line_back(n - at + 1);
  return n;
}

/* The cursor is the console's own: `at` is where it sits in the line, and
 * every edit reprints the tail from there and backspaces over it. Typing
 * inserts; at the end of the line, which is all typing ever did before Left,
 * Right, Home, End and Delete existed, the bytes echoed are what they were. */
int kbd_readline(char *buf, int max) {
  int n = 0;
  int at = 0;   /* the cursor, 0..n */
  int back = 0; /* 0 = the line being typed, 1.. = walking back */
  int i;

  for (;;) {
    int c;
    caret(1);
    c = kbd_getchar();
    caret(0);

    if (c < 0) break; /* the app is being killed; see kbd_getchar */
    if (c == '\n' || c == '\r') {
      line_show(buf, at, n);
      kcon_putc('\n');
      break;
    }
    if (c == '\b') {
      if (at) {
        kcon_putc('\b');
        n = line_delete(buf, n, --at);
      }
      continue;
    }
    if (c == SNOS_KEY_DELETE) {
      if (at < n) n = line_delete(buf, n, at);
      continue;
    }
    if (c == SNOS_KEY_LEFT) {
      if (at) {
        kcon_putc('\b');
        at--;
      }
      continue;
    }
    if (c == SNOS_KEY_RIGHT) {
      if (at < n) kcon_putc(buf[at++]);
      continue;
    }
    if (c == SNOS_KEY_HOME) {
      line_back(at);
      at = 0;
      continue;
    }
    if (c == SNOS_KEY_END) {
      line_show(buf, at, n);
      at = n;
      continue;
    }
    if (c == SNOS_KEY_UP) {
      const char *h = hist_get(back + 1);
      if (h) {
        back++;
        line_show(buf, at, n);
        at = n = line_replace(buf, n, h, max);
      }
      continue;
    }
    if (c == SNOS_KEY_DOWN) {
      if (back) line_show(buf, at, n);
      if (back > 1) {
        back--;
        at = n = line_replace(buf, n, hist_get(back), max);
      } else if (back == 1) {
        back = 0;
        at = n = line_replace(buf, n, 0, max); /* back to an empty line */
      }
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;
    if (n >= max - 1) continue;
    for (i = n; i > at; i--) buf[i] = buf[i - 1];
    buf[at] = (char)c;
    n++;
    line_show(buf, at, n);
    at++;
    line_back(n - at);
  }

  buf[n] = 0;
  hist_add(buf);
  return n;
}
