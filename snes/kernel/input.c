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

uint16_t pad_held(void) { return pad_cur; }

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

/* ------------------------------------------------------------------ OSK */

/* Six rows across the bottom: a framed panel with the key grid on the left and
 * the controller mapping on the right, so the controls are visible exactly when
 * you need them rather than buried in `help`. */
#define PANEL_ROWS 6
#define PANEL_TOP (28 - PANEL_ROWS)
#define KEY_ROWS 4
#define KEY_COL0 3   /* left edge of the key grid */
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

static uint8_t panel_buf[64];
static uint8_t panel_dirty;
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

static void put(uint8_t *b, int col, const char *s) {
  while (*s && col < 63) b[col++] = (uint8_t)*s++;
}

static void build_row(uint8_t *b, int prow) {
  int i;

  for (i = 0; i < 64; i++) b[i] = ' ';

  /* Frame */
  if (prow == 0) {
    for (i = 1; i < 63; i++) b[i] = CON_BOX_H;
    b[0] = CON_BOX_TL;
    b[63] = CON_BOX_TR;
    return;
  }
  if (prow == PANEL_ROWS - 1) {
    for (i = 1; i < 63; i++) b[i] = CON_BOX_H;
    b[0] = CON_BOX_BL;
    b[63] = CON_BOX_BR;
    return;
  }
  b[0] = CON_BOX_V;
  b[63] = CON_BOX_V;

  {
    int krow = prow - 1;
    const char *s = osk_shift ? KEYS_HI[krow] : KEYS_LO[krow];

    /* Stagger each row by one column, the way a real keyboard is offset. */
    for (i = 0; s[i]; i++) {
      int c = KEY_COL0 + krow + i * KEY_STEP;
      if (c >= 62) break;
      /* $80 selects the inverse-video twin of the glyph — the selection
       * highlight, since Mode 5 has no spare layer to draw a cursor on. */
      b[c] = (uint8_t)(s[i] | ((krow == sel_row && i == sel_col) ? 0x80 : 0x00));
    }
    put(b, HINT_COL, HINTS[krow]);
  }
}

void osk_lock(int on) { osk_locked = on; }

void osk_show(void) {
  osk_on = 1;
  panel_dirty = (1 << PANEL_ROWS) - 1;
  con_split_enable(PANEL_TOP);
}

void osk_hide(void) {
  osk_on = 0;
  panel_dirty = 0;
  con_split_disable();
}

int osk_visible(void) { return osk_on; }

/* One row per call, so a repaint never overruns a vblank. Only panel changes
 * cost anything now — scrolling text no longer disturbs it at all, which is
 * the entire point of the split. */
void osk_redraw(void) {
  int i;
  if (!osk_on || !panel_dirty) return;
  for (i = 0; i < PANEL_ROWS; i++) {
    if (panel_dirty & (1 << i)) {
      build_row(panel_buf, i);
      con_panel_row(PANEL_TOP + i, panel_buf);
      panel_dirty &= (uint8_t)~(1 << i);
      return;
    }
  }
}

static void move_sel(int dr, int dc) {
  int old = sel_row;
  if (dr) {
    sel_row = (sel_row + dr + KEY_ROWS) % KEY_ROWS;
    clamp_sel();
    panel_dirty |= (uint8_t)((1 << (old + 1)) | (1 << (sel_row + 1)));
  }
  if (dc) {
    int n = keyrow_len(sel_row);
    sel_col = (sel_col + dc + n) % n;
    panel_dirty |= (uint8_t)(1 << (sel_row + 1));
  }
}

int osk_poll(void) {
  uint16_t p = pad_pressed();
  uint16_t h = pad_held();

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
    panel_dirty |= 0x1E; /* all four key rows */
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

int kbd_getchar_nonblock(void) {
  int c = mb_getc_nonblock(); /* the harness types here */
  if (c >= 0) return c;
  return osk_poll();
}

void kbd_idle(void) {
  uint16_t f = snos_frames;
  while (snos_frames == f) {
  }
  osk_redraw();
  con_flush();
}

int kbd_getchar(void) {
  for (;;) {
    int c = kbd_getchar_nonblock();
    if (c >= 0) return c;
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

/* Wipe what is on screen and put `src` there instead.
 *
 * Erasing with backspaces cannot cross a row boundary — con_putc's backspace
 * stops at column 0 — so a line that wrapped would leave its first row behind.
 * Shell commands are far shorter than 64 columns in practice, and the
 * alternative is repainting the whole row from the shadow buffer, which the
 * line editor has no business reaching into. Noted rather than solved. */
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

int kbd_readline(char *buf, int max) {
  int n = 0;
  int back = 0; /* 0 = the line being typed, 1.. = walking back */

  for (;;) {
    int c;
    caret(1);
    c = kbd_getchar();
    caret(0);

    if (c == '\n' || c == '\r') {
      kcon_putc('\n');
      break;
    }
    if (c == '\b') {
      if (n) {
        n--;
        kcon_putc('\b');
        kcon_putc(' ');
        kcon_putc('\b');
      }
      continue;
    }
    if (c == SNOS_KEY_UP) {
      const char *h = hist_get(back + 1);
      if (h) {
        back++;
        n = line_replace(buf, n, h, max);
      }
      continue;
    }
    if (c == SNOS_KEY_DOWN) {
      if (back > 1) {
        back--;
        n = line_replace(buf, n, hist_get(back), max);
      } else if (back == 1) {
        back = 0;
        n = line_replace(buf, n, 0, max); /* back to an empty line */
      }
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;
    if (n >= max - 1) continue;
    buf[n++] = (char)c;
    kcon_putc(c);
  }

  buf[n] = 0;
  hist_add(buf);
  return n;
}
