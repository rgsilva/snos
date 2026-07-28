/* SNOS input layer: controller, on-screen keyboard, line editor.
 *
 * The OSK is drawn with SPRITES, as a bordered panel floating in the middle of
 * the screen.
 *
 * It began as text written into the console's bottom rows, which was wrong in a
 * way no amount of care would have fixed. The console scrolls the background
 * with the hardware scroll registers, so the panel scrolled along with the
 * text: keyboard rows drifted up into the output, and typing near the bottom
 * corrupted the display. Moving it elsewhere on the background would only have
 * changed where it drifted from.
 *
 * Sprites are addressed in absolute screen coordinates and are untouched by
 * background scrolling, so the whole class of bug disappears. Nothing needs
 * saving or restoring either — hiding the panel is one register write.
 */
#include "input.h"
#include "console.h"
#include "sprite.h"
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

/* A 14x6 grid of 8x8 sprite cells: a one-cell border around four rows of at
 * most twelve keys. 84 sprites, and at most 14 on any scanline, which is
 * comfortably inside the SNES limits of 128 total and 32 per line. */
#define PANEL_W 14
#define PANEL_H 6
#define KEY_ROWS 4
#define KEYS_MAX 12

/* Centred: the screen is 256x224 in sprite coordinates. */
#define PANEL_X ((256 - PANEL_W * 8) / 2)
#define PANEL_Y ((224 - PANEL_H * 8) / 2)

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

static uint8_t panel[PANEL_W * PANEL_H];
static uint8_t panel_dirty;
static int osk_on = 1;      /* visible by default: on a stock SNES it is the
                             * only way to type, so starting hidden would leave
                             * a new user with no way in. */
static int osk_shift;
static int sel_row = 1, sel_col;
static uint16_t repeat_at;

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

static void build_panel(void) {
  int r, c;
  for (r = 0; r < PANEL_H; r++) {
    for (c = 0; c < PANEL_W; c++) {
      uint8_t v;
      if (r == 0 || r == PANEL_H - 1 || c == 0 || c == PANEL_W - 1) {
        v = SPR_BORDER;
      } else {
        int kr = r - 1, kc = c - 1;
        const char *s = osk_shift ? KEYS_HI[kr] : KEYS_LO[kr];
        if (kc < keyrow_len(kr)) {
          v = (uint8_t)s[kc];
          if (kr == sel_row && kc == sel_col) v |= SPR_SELECTED;
        } else {
          v = SPR_PAD;
        }
      }
      panel[r * PANEL_W + c] = v;
    }
  }
}

void osk_show(void) {
  osk_on = 1;
  panel_dirty = 1;
}

void osk_hide(void) {
  osk_on = 0;
  spr_enable(0);
}

int osk_visible(void) { return osk_on; }

/* Rebuild and upload in one go. The whole panel is 544 bytes of OAM, which is
 * nothing in a vblank, so there is no need for the per-row dirty tracking the
 * background version needed. */
void osk_redraw(void) {
  if (!osk_on || !panel_dirty) return;
  build_panel();
  spr_panel(panel, PANEL_W, PANEL_H, PANEL_X, PANEL_Y);
  spr_commit();
  spr_enable(1);
  panel_dirty = 0;
}

static void move_sel(int dr, int dc) {
  if (dr) {
    sel_row = (sel_row + dr + KEY_ROWS) % KEY_ROWS;
    clamp_sel();
  }
  if (dc) {
    int n = keyrow_len(sel_row);
    sel_col = (sel_col + dc + n) % n;
  }
  panel_dirty = 1;
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
    if (osk_on) osk_hide();
    else osk_show();
    return -1;
  }
  if (p & PAD_START) return '\n';
  if (p & PAD_B) return '\b';
  if (p & PAD_X) return ' ';

  if (!osk_on) return -1;

  if (p & PAD_Y) {
    osk_shift = !osk_shift;
    clamp_sel();
    panel_dirty = 1;
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
  spr_init();
  osk_show();
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

static void caret(int on) {
  int r, c;
  con_getpos(&r, &c);
  con_putat(r, c, on ? (0x80 | ' ') : ' ');
}

int kbd_readline(char *buf, int max) {
  int n = 0;
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
    if (c < 0x20 || c > 0x7E) continue;
    if (n >= max - 1) continue;
    buf[n++] = (char)c;
    kcon_putc(c);
  }
  buf[n] = 0;
  return n;
}
