/* SNOS EDIT — a menu-driven full-screen text editor.
 *
 * In the spirit of snes86's TED, but native: a SNOS application in its own ROM
 * bank, editing files on the cartridge's battery-backed SRAM volume through
 * system calls.
 *
 * ------------------------------------------------------------- the input problem
 *
 * Typing on this machine is expensive, and the d-pad is contended. While the
 * on-screen keyboard is up it OWNS the d-pad — that is how you move between
 * keys — so the editor cannot also use it to move the text cursor. Rather than
 * invent a third thing for the user to learn, the editor leans on the toggle
 * they already know:
 *
 *   keyboard SHOWN   d-pad picks letters, you are typing
 *   keyboard HIDDEN  d-pad moves the text cursor, you are navigating
 *
 * SELECT toggles it, exactly as it does in the shell, and the status bar always
 * says which mode you are in. With the host keyboard bridge the keyboard can
 * stay hidden and both work at once.
 *
 * L opens the menu from either mode — it is the one button the kernel never
 * intercepts. The menu hides the keyboard for the duration (SYS_OSK) so the
 * d-pad is free to navigate it, then puts it back.
 *
 * ------------------------------------------------------------------ storage
 *
 * 192 lines of at most 63 characters, one fixed-width slot each: 12 KB of the
 * 64 KB application arena. Fixed slots make the common operation — typing a
 * character — a move of at most 63 bytes, and pay for it on the rare one,
 * splitting or joining a line, which shifts the array. A gap buffer would
 * reverse that trade; for notes and small BASIC programs this is the right way
 * round. 63 characters is not arbitrary either: the console is 64 columns, so a
 * full line still leaves a column for the cursor to sit in.
 */
#include "snos_abi.h"

#define MAXLINES 192
#define LINECAP 64  /* 63 characters plus the terminator */
#define LINEMAX (LINECAP - 1)
#define NAMEMAX 16

/* ------------------------------------------------------------------ state */

static char text[MAXLINES][LINECAP];
static uint8_t linelen[MAXLINES];
static uint16_t nlines;

static uint16_t cur_line, cur_col, top_line;
static char filename[NAMEMAX + 2];
static uint8_t dirty;          /* unsaved changes */
static uint8_t quit_flag;

static uint16_t scr_rows, scr_cols; /* usable console geometry, re-read often */
static char rowbuf[80];
static char msg[48];

/* Menu / picker state */
static uint16_t menu_sel;
static uint16_t pick_sel, pick_count;
static char pickname[NAMEMAX + 2];

/* ---------------------------------------------------------------- helpers */

static void strcopy(char *d, const char *s, int cap) {
  int i = 0;
  while (s[i] && i < cap - 1) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static int strlength(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void setmsg(const char *s) { strcopy(msg, s, (int)sizeof msg); }

/* ---------------------------------------------------------------- drawing */

static void pad_row(int from) {
  int i;
  for (i = from; i < (int)scr_cols; i++) rowbuf[i] = ' ';
  rowbuf[scr_cols] = 0;
}

/* Inverse video: characters $80-$FF are the console font's inverted twins, and
 * with both BG layers carrying text there is no spare layer to highlight with. */
static void invert(int from, int to) {
  int i;
  for (i = from; i < to && i < (int)scr_cols; i++)
    rowbuf[i] = (char)((unsigned char)rowbuf[i] | 0x80u);
}

static void put_at(int col, const char *s) {
  int i = 0;
  while (s[i] && col + i < (int)scr_cols) {
    rowbuf[col + i] = s[i];
    i++;
  }
}

/* Both return the column just past what they wrote, so the status bar can be
 * laid out left to right instead of at guessed fixed offsets — which is what
 * produced "ln 2    /2". */
static int put_run(int col, const char *s) {
  int i = 0;
  while (s[i] && col + i < (int)scr_cols) {
    rowbuf[col + i] = s[i];
    i++;
  }
  return col + i;
}

static int put_num(int col, uint16_t v) {
  char t[6];
  int n = 0, i;
  if (v == 0) t[n++] = '0';
  while (v && n < 5) {
    t[n++] = (char)('0' + (v % 10u));
    v = (uint16_t)(v / 10u);
  }
  for (i = 0; i < n; i++)
    if (col + i < (int)scr_cols) rowbuf[col + i] = t[n - 1 - i];
  return col + n;
}

static void draw_status(void) {
  int c;
  pad_row(0);

  c = put_run(1, filename[0] ? filename : "(new)");
  if (dirty) put_run(c, "*");

  /* Both ways in are advertised: the button for someone on a controller, the
   * control key for someone on the host keyboard bridge. */
  put_run(18, "Esc/^L menu");

  /* The d-pad is shared with the on-screen keyboard, so say which one has it. */
  put_run(31, sys_osk(2) ? "type" : "move");

  c = put_run(36, "ln ");
  c = put_num(c, (uint16_t)(cur_line + 1));
  c = put_run(c, "/");
  c = put_num(c, nlines);
  c = put_run(c + 2, "col ");
  put_num(c, (uint16_t)(cur_col + 1));

  if (msg[0]) put_at(54, msg);
  invert(0, (int)scr_cols);
  sys_puts_at(0, 0, rowbuf);
}

static void draw_text_row(int sr) {
  uint16_t li = (uint16_t)(top_line + sr - 1);
  int i, n;
  pad_row(0);
  if (li < nlines) {
    n = linelen[li];
    for (i = 0; i < n && i < (int)scr_cols; i++) rowbuf[i] = text[li][i];
    if (li == cur_line && cur_col < scr_cols)
      rowbuf[cur_col] = (char)((unsigned char)rowbuf[cur_col] | 0x80u);
  } else if (li == nlines) {
    rowbuf[0] = '~';
  }
  rowbuf[scr_cols] = 0;
  sys_puts_at(sr, 0, rowbuf);
}

static void draw_all(void) {
  int sr;
  draw_status();
  for (sr = 1; sr < (int)scr_rows; sr++) draw_text_row(sr);
}

/* Keep the cursor line on screen. Returns 1 if the view moved, so the caller
 * knows a full repaint is needed rather than a one-line touch-up. */
static int scroll_into_view(void) {
  uint16_t body = (uint16_t)(scr_rows - 1);
  uint16_t old = top_line;
  if (cur_line < top_line) top_line = cur_line;
  else if (cur_line >= top_line + body) top_line = (uint16_t)(cur_line - body + 1);
  return top_line != old;
}

static int cursor_screen_row(void) { return (int)(cur_line - top_line) + 1; }

/* ------------------------------------------------------------ text editing */

static void buffer_new(void) {
  int i;
  for (i = 0; i < MAXLINES; i++) linelen[i] = 0;
  for (i = 0; i < LINECAP; i++) text[0][i] = 0;
  nlines = 1;
  cur_line = 0;
  cur_col = 0;
  top_line = 0;
  dirty = 0;
  filename[0] = 0;
}

static void line_insert_at(uint16_t at) {
  uint16_t i;
  int j;
  if (nlines >= MAXLINES) return;
  for (i = nlines; i > at; i--) {
    for (j = 0; j < LINECAP; j++) text[i][j] = text[i - 1][j];
    linelen[i] = linelen[i - 1];
  }
  linelen[at] = 0;
  text[at][0] = 0;
  nlines++;
}

static void line_delete_at(uint16_t at) {
  uint16_t i;
  int j;
  if (nlines <= 1) {
    linelen[0] = 0;
    text[0][0] = 0;
    cur_col = 0;
    return;
  }
  for (i = at; i + 1 < nlines; i++) {
    for (j = 0; j < LINECAP; j++) text[i][j] = text[i + 1][j];
    linelen[i] = linelen[i + 1];
  }
  nlines--;
  if (cur_line >= nlines) cur_line = (uint16_t)(nlines - 1);
}

static void insert_char(char c) {
  uint8_t n = linelen[cur_line];
  int i;
  if (n >= LINEMAX) {
    setmsg("line full");
    return;
  }
  for (i = n; i > (int)cur_col; i--) text[cur_line][i] = text[cur_line][i - 1];
  text[cur_line][cur_col] = c;
  linelen[cur_line] = (uint8_t)(n + 1);
  text[cur_line][n + 1] = 0;
  cur_col++;
  dirty = 1;
}

static void split_line(void) {
  uint8_t n = linelen[cur_line];
  int i, k = 0;
  if (nlines >= MAXLINES) {
    setmsg("too many lines");
    return;
  }
  line_insert_at((uint16_t)(cur_line + 1));
  for (i = (int)cur_col; i < n; i++) text[cur_line + 1][k++] = text[cur_line][i];
  text[cur_line + 1][k] = 0;
  linelen[cur_line + 1] = (uint8_t)k;
  linelen[cur_line] = (uint8_t)cur_col;
  text[cur_line][cur_col] = 0;
  cur_line++;
  cur_col = 0;
  dirty = 1;
}

static void backspace(void) {
  int i;
  if (cur_col > 0) {
    uint8_t n = linelen[cur_line];
    for (i = (int)cur_col - 1; i < n - 1; i++) text[cur_line][i] = text[cur_line][i + 1];
    linelen[cur_line] = (uint8_t)(n - 1);
    text[cur_line][n - 1] = 0;
    cur_col--;
    dirty = 1;
    return;
  }
  if (cur_line == 0) return;
  {
    /* Join with the previous line, as far as it will fit. */
    uint16_t prev = (uint16_t)(cur_line - 1);
    uint8_t pn = linelen[prev], n = linelen[cur_line];
    int k = 0;
    while (k < n && pn + k < LINEMAX) {
      text[prev][pn + k] = text[cur_line][k];
      k++;
    }
    linelen[prev] = (uint8_t)(pn + k);
    text[prev][pn + k] = 0;
    line_delete_at(cur_line);
    cur_line = prev;
    cur_col = pn;
    dirty = 1;
  }
}

/* ------------------------------------------------------------------ files */

static int file_load(const char *name) {
  int c, col = 0;
  uint16_t li = 0;

  snos_set_ptr(name);
  if (sys_open(0) == SYS_ERR) return -1;

  for (li = 0; li < MAXLINES; li++) linelen[li] = 0;
  li = 0;
  text[0][0] = 0;

  for (;;) {
    c = (int)sys_getc_f();
    if (c == (int)SYS_ERR) break;
    if (c == '\n') {
      text[li][col] = 0;
      linelen[li] = (uint8_t)col;
      col = 0;
      li++;
      if (li >= MAXLINES) {
        li = MAXLINES - 1;
        break;
      }
      text[li][0] = 0;
      continue;
    }
    if (c < 0x20 || c > 0x7E) continue;
    if (col < LINEMAX) {
      text[li][col++] = (char)c;
      text[li][col] = 0;
      linelen[li] = (uint8_t)col;
    }
  }
  sys_close();

  /* A trailing newline closes the last line; without one, whatever was being
   * accumulated is still a line. */
  if (col > 0) li++;
  nlines = li ? li : 1;
  cur_line = 0;
  cur_col = 0;
  top_line = 0;
  dirty = 0;
  strcopy(filename, name, sizeof filename);
  return 0;
}

static int file_save(const char *name) {
  uint16_t i;
  int j;
  int err = 0;

  snos_set_ptr(name);
  if (sys_open(1) == SYS_ERR) return -1;
  /* Checked per byte: a volume that fills mid-save otherwise truncates the
   * document while the status bar says 'saved', dirty is cleared, and the
   * quit prompt never fires -- a silent data loss with an on-screen receipt. */
  for (i = 0; i < nlines && !err; i++) {
    for (j = 0; j < linelen[i] && !err; j++)
      err = sys_putc_f((uint16_t)(unsigned char)text[i][j]) == SYS_ERR;
    if (!err) err = sys_putc_f('\n') == SYS_ERR;
  }
  sys_close();
  if (err) {
    /* Remove the truncated copy; keep dirty set so quitting still warns. */
    snos_set_ptr(name);
    sys_unlink();
    return -1;
  }
  strcopy(filename, name, sizeof filename);
  dirty = 0;
  return 0;
}

/* ------------------------------------------------------------------ input */

/* The editor does its own edge detection on the HELD bitmask rather than
 * relying on the kernel's, because the kernel consumes button edges for the
 * on-screen keyboard and the editor still needs to see the same presses. */
static uint16_t pad_prev;
static uint16_t rep_mask;
static uint16_t rep_at;

static uint16_t pad_edges(void) {
  uint16_t now = sys_pad();
  uint16_t e = (uint16_t)(now & ~pad_prev);
  uint16_t dirs = (uint16_t)(now & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT));
  uint16_t frames = sys_frames();

  pad_prev = now;

  /* Auto-repeat on the d-pad only: crossing a document one press at a time is
   * as miserable here as crossing the keyboard was. */
  if (dirs) {
    if (rep_mask != dirs) {
      rep_mask = dirs;
      rep_at = (uint16_t)(frames + 22u);
    } else if ((int16_t)(frames - rep_at) >= 0) {
      rep_at = (uint16_t)(frames + 4u);
      e |= dirs;
    }
  } else {
    rep_mask = 0;
  }
  return e;
}

static void drain_keys(void) {
  while (sys_pollkey() != SYS_ERR) {
  }
}

/* ------------------------------------------------------------------- menu */

#define MENU_ITEMS 7
static const char *const MENU[MENU_ITEMS] = {
    "New          ^N", "Open...      ^O", "Save         ^S",
    "Save As...   ^A", "Delete Line  ^K", "Go To Top",
    "Quit         ^Q",
};
#define MENU_W 20
#define MENU_ROW 2
#define MENU_COL 3

/* One item row. Split out because moving the selection changes exactly two of
 * them, and the console charges by the ROW: a dirty row costs about a frame and
 * a half whatever is on it, so repainting the whole box to move a highlight was
 * paying nine rows for two rows of work. */
static void draw_menu_item(int i) {
  pad_row(0);
  rowbuf[MENU_COL] = SNOS_BOX_V;
  rowbuf[MENU_COL + MENU_W - 1] = SNOS_BOX_V;
  put_at(MENU_COL + 2, MENU[i]);
  if (i == (int)menu_sel) invert(MENU_COL + 1, MENU_COL + MENU_W - 1);
  rowbuf[scr_cols] = 0;
  sys_puts_at(MENU_ROW + 1 + i, 0, rowbuf);
}

static void draw_menu(void) {
  int i, j;

  pad_row(0);
  rowbuf[MENU_COL] = SNOS_BOX_TL;
  for (j = 1; j < MENU_W - 1; j++) rowbuf[MENU_COL + j] = SNOS_BOX_H;
  rowbuf[MENU_COL + MENU_W - 1] = SNOS_BOX_TR;
  rowbuf[scr_cols] = 0;
  sys_puts_at(MENU_ROW, 0, rowbuf);

  for (i = 0; i < MENU_ITEMS; i++) draw_menu_item(i);

  pad_row(0);
  rowbuf[MENU_COL] = SNOS_BOX_BL;
  for (j = 1; j < MENU_W - 1; j++) rowbuf[MENU_COL + j] = SNOS_BOX_H;
  rowbuf[MENU_COL + MENU_W - 1] = SNOS_BOX_BR;
  rowbuf[scr_cols] = 0;
  sys_puts_at(MENU_ROW + 1 + MENU_ITEMS, 0, rowbuf);
}

/* ------------------------------------------------------------ modal prompts */

/* A one-line prompt drawn on the status row, with its own tiny line editor.
 * Deliberately not SYS_READLINE: that draws at the console's own cursor and can
 * scroll, which would tear a full-screen layout apart. Returns 0 if cancelled
 * (empty input, or L). */
static int prompt(const char *label, char *out, int cap) {
  int n = 0;
  int was_osk = (int)sys_osk(2);

  sys_osk(1); /* you are about to type: put the keyboard up */
  out[0] = 0;

  {
  int need = 1;
  for (;;) {
    int c;
    uint16_t e;

    if (!need) goto prompt_input;
    need = 0;
    pad_row(0);
    put_at(1, label);
    put_at(1 + strlength(label) + 1, out);
    {
      int cc = 1 + strlength(label) + 1 + n;
      if (cc < (int)scr_cols) rowbuf[cc] = (char)((unsigned char)rowbuf[cc] | 0x80u);
    }
    invert(0, 1);
    sys_puts_at(0, 0, rowbuf);

prompt_input:
    sys_yield();

    e = pad_edges();
    if (e & PAD_L) {
      if (!was_osk) sys_osk(0);
      return 0;
    }

    while ((c = (int)sys_pollkey()) != (int)SYS_ERR) {
      if (c == '\n') {
        if (!was_osk) sys_osk(0);
        return n > 0;
      }
      if (c == '\b') {
        if (n > 0) out[--n] = 0;
        need = 1;
        continue;
      }
      if (c < 0x20 || c > 0x7E) continue;
      if (n < cap - 1) {
        out[n++] = (char)c;
        out[n] = 0;
        need = 1;
      }
    }
  }
  }
}

/* A yes/no box. A or Y confirms, B or N cancels. */
static int confirm(const char *question) {
  int was_osk = (int)sys_osk(2);
  sys_osk(0); /* free the d-pad and stop A/B typing into the document */

  pad_row(0);
  put_at(1, question);
  put_at(1 + strlength(question) + 2, "A/y = yes   B/n = no");
  invert(0, (int)scr_cols);
  sys_puts_at(0, 0, rowbuf);

  for (;;) {
    uint16_t e;
    sys_yield();

    /* Answerable from the keyboard too. A dialog that only a game controller
     * can dismiss is no use to someone typing on the host bridge. */
    {
      int c;
      while ((c = (int)sys_pollkey()) != (int)SYS_ERR) {
        if (c == 'y' || c == 'Y') {
          if (was_osk) sys_osk(1);
          return 1;
        }
        if (c == 'n' || c == 'N' || c == 0x1B) {
          if (was_osk) sys_osk(1);
          return 0;
        }
      }
    }

    e = pad_edges();
    if (e & PAD_A) {
      if (was_osk) sys_osk(1);
      return 1;
    }
    if (e & (PAD_B | PAD_L)) {
      if (was_osk) sys_osk(1);
      return 0;
    }
  }
}

/* --------------------------------------------------------------- file pick */

static int pick_file(void) {
  int was_osk = (int)sys_osk(2);
  uint16_t i;

  sys_osk(0);
  pick_sel = 0;

  /* SYS_DIR walks the whole namespace, ROM files included. */
  pick_count = 0;
  for (i = 0; i < 64; i++) {
    snos_set_ptr(pickname);
    if (sys_dir(i) == 0) break;
    pick_count++;
  }
  if (pick_count == 0) {
    setmsg("no files");
    if (was_osk) sys_osk(1);
    return 0;
  }

  {
  int need = 1;
  for (;;) {
    uint16_t e;
    uint16_t shown = (uint16_t)(scr_rows - 2);
    uint16_t first = 0;

    if (shown > pick_count) shown = pick_count;
    if (pick_sel >= shown) first = (uint16_t)(pick_sel - shown + 1);

    if (!need) goto pick_input; /* see draw_menu: repainting every frame starves
                                 * the console and the list never finishes */
    need = 0;
    pad_row(0);
    put_at(1, "Open  -  Enter=open  Esc=cancel");
    invert(0, (int)scr_cols);
    sys_puts_at(0, 0, rowbuf);

    for (i = 0; i < shown; i++) {
      snos_set_ptr(pickname);
      sys_dir((uint16_t)(first + i));
      pad_row(0);
      put_at(2, pickname);
      if (first + i == pick_sel) invert(1, 24);
      rowbuf[scr_cols] = 0;
      sys_puts_at((int)i + 1, 0, rowbuf);
    }
    for (i = shown; i < (uint16_t)(scr_rows - 1); i++) {
      pad_row(0);
      sys_puts_at((int)i + 1, 0, rowbuf);
    }

pick_input:
    sys_yield();

    /* Key-driven, like the menu: the kernel reports the d-pad as arrow codes
     * while the keyboard is hidden, so a controller and a host keyboard arrive
     * here identically. Reading pad edges as well would move the selection
     * twice per press. */
    {
      int c;
      while ((c = (int)sys_pollkey()) != (int)SYS_ERR) {
        if (c == SNOS_KEY_UP) {
          if (pick_sel) {
            pick_sel--;
            need = 1;
          }
          continue;
        }
        if (c == SNOS_KEY_DOWN) {
          if (pick_sel + 1 < pick_count) {
            pick_sel++;
            need = 1;
          }
          continue;
        }
        if (c == '\n' || c == '\r') {
          snos_set_ptr(pickname);
          sys_dir(pick_sel);
          if (was_osk) sys_osk(1);
          if (file_load(pickname) != 0) {
            setmsg("open failed");
            return 0;
          }
          setmsg("opened");
          return 1;
        }
        if (c == 0x1B || c == '\b') { /* Escape, or B on a controller */
          if (was_osk) sys_osk(1);
          setmsg("cancelled");
          return 0;
        }
      }
    }
  }
  }
}

/* -------------------------------------------------------------- menu actions */

static void act_save_as(void) {
  char name[NAMEMAX + 2];
  if (!prompt("Save as:", name, (int)sizeof name)) {
    setmsg("cancelled");
    return;
  }
  if (file_save(name) != 0) setmsg("save failed");
  else setmsg("saved");
}

static void act_save(void) {
  if (filename[0] == 0) {
    act_save_as();
    return;
  }
  if (file_save(filename) != 0) setmsg("save failed");
  else setmsg("saved");
}

static void act_quit(void) {
  if (dirty && !confirm("Unsaved changes. Quit?")) {
    setmsg("");
    return;
  }
  quit_flag = 1;
}

static void act_new(void) {
  if (!dirty || confirm("Unsaved changes. Discard?")) {
    buffer_new();
    setmsg("new file");
  }
}

static void act_open(void) {
  if (!dirty || confirm("Unsaved changes. Discard?")) pick_file();
}

static void act_delete_line(void) {
  line_delete_at(cur_line);
  cur_col = 0;
  dirty = 1;
  setmsg("line deleted");
}

/* Control characters, so everything reachable from the menu is also reachable
 * from the keyboard. The menu exists for discoverability; these exist because
 * driving an editor through a game controller for real work is miserable, and
 * the host keyboard bridge is how anyone will actually use this. Returns 1 if
 * the character was a command. */
static int shortcut(int c) {
  switch (c) {
  case 0x0E: act_new(); return 1;          /* ^N */
  case 0x0F: act_open(); return 1;         /* ^O */
  case 0x13: act_save(); return 1;         /* ^S */
  case 0x01: act_save_as(); return 1;      /* ^A */
  case 0x0B: act_delete_line(); return 1;  /* ^K */
  case 0x11: act_quit(); return 1;         /* ^Q */
  default: return 0;
  }
}

static void run_menu(void) {
  int was_osk = (int)sys_osk(2);
  int need = 1;          /* the whole box: only on opening */
  int moved_from = -1;   /* or just the two rows a moved highlight touches */

  /* The keyboard owns the d-pad while it is up, so it has to go away for the
   * menu to be navigable at all. */
  sys_osk(0);
  menu_sel = 0;

  /* Driven ENTIRELY by keys, including the d-pad.
   *
   * With the keyboard hidden the kernel reports the d-pad as SNOS_KEY_UP and
   * friends, so a controller and a host keyboard deliver exactly the same
   * codes here and there is nothing device-specific left to write. Reading pad
   * edges as well — which this used to do — would now move the selection twice
   * per press, once for the arrow code and once for the button. */
  for (;;) {
    int c;

    /* Redraw ONLY on change. Repainting every frame re-dirties all nine rows
     * while the console drains two per vblank, so the box never finishes
     * appearing — it showed three items and no bottom border, indefinitely. */
    if (need) {
      draw_menu();
      need = 0;
    } else if (moved_from >= 0) {
      /* Two rows instead of nine. The console charges per dirty row, so this is
       * the difference between a highlight that moves when you press the key
       * and one that arrives a moment later. */
      draw_menu_item(moved_from);
      draw_menu_item((int)menu_sel);
      moved_from = -1;
    }
    sys_yield();

    while ((c = (int)sys_pollkey()) != (int)SYS_ERR) {
      if (c == 0x0C || c == 0x1B) { /* ^L or Escape closes */
        if (was_osk) sys_osk(1);
        return;
      }
      if (c == SNOS_KEY_UP) {
        /* Remember where it came FROM the first time only: several key events
         * can arrive before the next repaint, and the row that still shows a
         * stale highlight is the one we started at, not the one we passed
         * through. */
        if (moved_from < 0) moved_from = (int)menu_sel;
        menu_sel = (uint16_t)((menu_sel + MENU_ITEMS - 1) % MENU_ITEMS);
        continue;
      }
      if (c == SNOS_KEY_DOWN) {
        if (moved_from < 0) moved_from = (int)menu_sel;
        menu_sel = (uint16_t)((menu_sel + 1) % MENU_ITEMS);
        continue;
      }
      if (c == '\n' || c == '\r') { /* Enter, or START on a controller */
        switch (menu_sel) {
        case 0: act_new(); break;
        case 1: act_open(); break;
        case 2: act_save(); break;
        case 3: act_save_as(); break;
        case 4: act_delete_line(); break;
        case 5:
          cur_line = 0;
          cur_col = 0;
          top_line = 0;
          break;
        case 6: act_quit(); break;
        }
        if (was_osk) sys_osk(1);
        return;
      }
      /* The shortcuts printed beside each item work from inside the menu too,
       * so there is never a reason to navigate to something you can name. */
      if (shortcut(c)) {
        if (was_osk) sys_osk(1);
        return;
      }
      /* Anything else is swallowed, so a stray key cannot land in the document
       * behind the menu. */
    }
  }
}

/* ------------------------------------------------------------------- main */

static void refresh_geometry(void) {
  uint16_t g = sys_screen();
  scr_rows = (uint16_t)(g >> 8);
  scr_cols = (uint16_t)(g & 0xFFu);
  if (scr_cols > (uint16_t)(sizeof rowbuf - 2)) scr_cols = (uint16_t)(sizeof rowbuf - 2);
}

int main(void) {
  uint16_t last_rows;

  sys_cls();
  buffer_new();
  refresh_geometry();
  last_rows = scr_rows;
  setmsg("");

  /* ABI 4 launch argument: `run edit motd.txt` opens that file straight away.
   * A missing file is not an error worth refusing to start over — an empty
   * buffer under that name is what you wanted anyway, and is how every other
   * editor behaves. */
  {
    char arg[NAMEMAX + 2];
    if (snos_getargs(arg, (uint16_t)sizeof arg) > 0) {
      if (file_load(arg) == 0) {
        setmsg("opened");
      } else {
        int i;
        for (i = 0; arg[i] && i < NAMEMAX; i++) filename[i] = arg[i];
        filename[i] = 0;
        setmsg("new file");
      }
    }
  }

  sys_trace("EDIT-RAN\n");
  draw_all();

  while (!quit_flag) {
    int c;
    uint16_t e;
    int redraw_all = 0;
    int moved = 0;
    int old_row = cursor_screen_row();

    sys_yield();

    /* The usable height changes underneath us when the keyboard is toggled, so
     * it is asked for every frame rather than cached at startup. */
    refresh_geometry();
    if (scr_rows != last_rows) {
      last_rows = scr_rows;
      scroll_into_view();
      redraw_all = 1;
    }

    e = pad_edges();
    if (e & PAD_L) {
      run_menu();
      refresh_geometry();
      last_rows = scr_rows;
      scroll_into_view();
      redraw_all = 1;
      if (quit_flag) break;
    }

    /* Cursor movement arrives as arrow KEYS, below, not as pad edges.
     *
     * The kernel reports the d-pad as SNOS_KEY_UP..RIGHT whenever the on-screen
     * keyboard is hidden, so reading the buttons here as well would move the
     * cursor twice for every press. One path, and a host keyboard and a
     * controller are indistinguishable by the time they reach it. */
    if (!sys_osk(2) && (e & PAD_R)) { /* page down, the one spare button */
      uint16_t page = (uint16_t)(scr_rows - 2);
      cur_line = (uint16_t)(cur_line + page);
      if (cur_line >= nlines) cur_line = (uint16_t)(nlines - 1);
      if (cur_col > linelen[cur_line]) cur_col = linelen[cur_line];
      moved = 1;
    }

    while ((c = (int)sys_pollkey()) != (int)SYS_ERR) {
      if (c == '\n') {
        split_line();
        redraw_all = 1;
      } else if (c == '\b') {
        uint16_t before = cur_line;
        backspace();
        if (cur_line != before) redraw_all = 1;
      } else if (c == 0x0C || c == 0x1B) { /* ^L or Escape; L on a controller */
        run_menu();
        refresh_geometry();
        last_rows = scr_rows;
        redraw_all = 1;
      } else if (c == SNOS_KEY_UP) {
        if (cur_line) cur_line--;
      } else if (c == SNOS_KEY_DOWN) {
        if (cur_line + 1 < nlines) cur_line++;
      } else if (c == SNOS_KEY_LEFT) {
        if (cur_col) cur_col--;
        else if (cur_line) {
          cur_line--;
          cur_col = linelen[cur_line];
        }
      } else if (c == SNOS_KEY_RIGHT) {
        if (cur_col < linelen[cur_line]) cur_col++;
        else if (cur_line + 1 < nlines) {
          cur_line++;
          cur_col = 0;
        }
      } else if (shortcut(c)) {
        redraw_all = 1;
      } else if (c >= 0x20 && c <= 0x7E) {
        insert_char((char)c);
      }
      if (cur_col > linelen[cur_line]) cur_col = linelen[cur_line];
      moved = 1;
      if (quit_flag) break;
    }

    if (scroll_into_view()) redraw_all = 1;

    if (redraw_all) {
      draw_all();
    } else if (moved) {
      /* Only the lines the cursor left and arrived on can have changed, so a
       * keystroke costs two row updates instead of a full 22-row repaint —
       * which at two rows per vblank would be a fifth of a second each. */
      int nr = cursor_screen_row();
      if (old_row != nr && old_row >= 1 && old_row < (int)scr_rows) draw_text_row(old_row);
      if (nr >= 1 && nr < (int)scr_rows) draw_text_row(nr);
      draw_status();
    }
  }

  sys_cls();
  return 0;
}
