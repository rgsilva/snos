/* SNOS IRC — a Super Nintendo on IRC.
 *
 *     irc HOST[:PORT] [NICK] [#CHANNEL]
 *
 * Port defaults to 6667, nick to `snes`. A channel given on the command line is
 * joined once the server has finished registering us, not before — sending JOIN
 * ahead of the welcome numeric is answered with "you have not registered" on
 * every ircd there is.
 *
 * ------------------------------------------------------------------- layout
 *
 * A full-screen application, painted entirely with SYS_PUTS_AT:
 *
 *     row 0        tab bar, inverse video, active tab punched out of it
 *     rows 1..h-2  message pane
 *     row h-1      input line
 *
 * Nothing here ever calls SYS_PUTS. That is the whole reason the layout holds
 * still without an HDMA split: the console only scrolls when something writes
 * through the cursor, and SYS_PUTS_AT never does. The keyboard panel needs a
 * split because it coexists with a scrolling console; this program replaces the
 * console outright, so it does not.
 *
 * The consequence is that scrolling the message pane is a repaint of every pane
 * row rather than a hardware scroll — the console's own scroll moves whole rows
 * across the full width, which would take the tab bar and the input line with
 * it. Repainting 26 rows costs about seven vblanks (con_flush moves four rows
 * per vblank), and the kernel skips the link poll while the console is dirty, so
 * a repaint per arriving line would spend an MOTD burst painting instead of
 * receiving. Hence the rate limit in repaint(): the pane redraws at most every
 * PANE_MIN_FRAMES, the bar and the input line — one row each — go out
 * immediately, and typing stays responsive while a flood is arriving.
 *
 * The user list is an overlay over the pane for exactly the same reason. A side
 * panel would have to be repainted on every single message, because every
 * message moves every row.
 *
 * ----------------------------------------------------------------- protocol
 *
 * Deliberately NOT sent: `CAP LS`. Undernet and DALnet answer 421 (unknown
 * command) and QuakeNet answers with silence, so a client that waits for a
 * capability list hangs on three networks and there is nothing here that would
 * use a capability anyway.
 *
 * Deliberately NOT displayed: any PRIVMSG or NOTICE whose text starts with
 * \x01. OFTC sends an unsolicited CTCP VERSION the moment you connect, and
 * spraying `\x01VERSION\x01` into the message pane looks exactly like the link
 * corrupting data.
 *
 * PING is answered from the top of handle_line(), before anything else can
 * fail or return early. A missed PONG is not an error at the time — it is a
 * silent disconnection several minutes later, which is a miserable thing to
 * debug from the far end of a 615 B/s link.
 */
#include "snos_abi.h"
#include "snoslib.h"

#define LINE_W 64            /* the console is 64 columns; that is the ceiling */
#define MAXTABS 8
/* Scrollback per tab. 64 and not 40, measured against ergo: its stock MOTD is
 * 2.5 KB of ASCII art and colour charts, which wraps to about fifty lines at 64
 * columns — so at 40 the welcome numeric had already scrolled away by the time
 * the MOTD finished, and the first thing the server ever said to you was the
 * one thing you could not read. 8 x 64 x 65 is 33 KB of a 64 KB arena, which
 * leaves room for the user lists and the line buffers with a good margin. */
#define TABLINES 64
#define TABNAME_MAX 24
#define NICK_MAX 20
#define INPUT_MAX 200
#define RX_MAX 512           /* RFC 1459's line limit */
#define NET_BUF 128
#define OUT_MAX 560
#define USERS_MAX 48
#define USER_LEN 16

/* How long to wait for the gateway to report the connection open. Twenty
 * seconds is the gateway's own socket timeout, so anything past that is the
 * reply not crossing the link rather than the server being slow. */
#define OPEN_DEADLINE 1500u

/* Keepalive. Answering the server's PINGs (below) is what keeps the SERVER
 * from dropping us; this is the other direction. A dead bridge, a killed
 * gateway or a NAT that timed out all leave the channel looking OPEN while
 * nothing will ever arrive again — sys_net_state only notices honest TCP
 * death. So after ~3 minutes of total silence, ask, and give the answer ~30
 * seconds; ANY received line proves the pipe, not just the PONG. Both fit
 * comfortably in the (int16_t)(a - b) wrap idiom (< 32767 frames). */
#define KEEPALIVE_FRAMES 10800u
#define PONG_GRACE 1800u

/* Repaint pacing, and it matters more than it looks.
 *
 * con_flush moves four rows per vblank and a full pane is 26, so one repaint
 * occupies about seven frames — and the kernel skips the link poll on every
 * frame the console is still dirty (kernel/traps.c). Repainting per arriving
 * line therefore spends an MOTD painting intermediate states of a pane that is
 * about to change again, while the data it is painting arrives at a seventh of
 * the speed.
 *
 * So: while bytes are still coming in, hold the repaint off for as long as
 * PANE_MAX_DEFER; the moment the burst stops, paint promptly. A quiet channel
 * updates within PANE_MIN_FRAMES, which is the case a human is watching. */
#define PANE_MIN_FRAMES 8u
#define PANE_MAX_DEFER 45u

#define KEY_NEXT 0x0E  /* ^N */
#define KEY_PREV 0x10  /* ^P */
#define KEY_TABS 0x09  /* TAB */
#define KEY_USERS 0x15 /* ^U */
#define KEY_QUIT 0x11  /* ^Q, the same binding the editor uses */
#define KEY_ESC 0x1B

#define OV_NONE 0
#define OV_TABS 1
#define OV_USERS 2

static char args[SNOS_ARGV_MAX];
static char hostport[80];
static char nick[NICK_MAX + 1];
static char autojoin[TABNAME_MAX + 1];

static uint8_t nbuf[NET_BUF];
static char rx[RX_MAX + 1];
static uint16_t rxn;
static char outbuf[OUT_MAX];
static char msg[RX_MAX + 96];
static char wrapbuf[LINE_W + 1];
static char pbuf[LINE_W + 1];
static char cmdbuf[INPUT_MAX + 1];
static char pfxnick[NICK_MAX + 1];

static char input[INPUT_MAX + 1];
static uint16_t inn;

static char tabname[MAXTABS][TABNAME_MAX + 1];
static char lines[MAXTABS][TABLINES][LINE_W + 1];
static uint8_t lcount[MAXTABS];
static uint8_t lhead[MAXTABS];
static uint8_t tact[MAXTABS];
static uint8_t ntabs;
static uint8_t cur;

static char users[MAXTABS][USERS_MAX][USER_LEN + 1];
static uint8_t nusers[MAXTABS];

static uint16_t ch;
static uint16_t txmax;
static uint8_t rows, cols;
static uint8_t bar_dirty, pane_dirty, input_dirty, pane_force;
static uint16_t last_pane, pane_since;
static uint8_t overlay, ovsel, ovtop;
/* Lines the pane is held back from the newest. 0 is live. Without this the ring
 * is 64 lines deep and only ever 26 of them are reachable, which is the same as
 * not having it — a server whose MOTD is longer than a screen would push its
 * own welcome out of sight before you could read it. */
static uint16_t scroll;
static uint8_t quitting, registered, disconnected, nick_tries;
/* Keepalive state: frame of the last received byte, and — once a probe is in
 * flight — the frame by which something must arrive back. */
static uint16_t last_rx, pong_by;
static uint8_t ping_sent;

/* ---------------------------------------------------------------- strings */

static uint16_t slen(const char *s) {
  uint16_t n = 0;
  while (s[n]) n++;
  return n;
}

static void scpy(char *d, const char *s, uint16_t max) {
  uint16_t i = 0;
  while (i + 1 < max && s[i]) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static void scat(char *d, const char *s, uint16_t max) {
  uint16_t i = slen(d), j = 0;
  while (i + 1 < max && s[j]) d[i++] = s[j++];
  d[i] = 0;
}

static char lowerc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* IRC names are case-insensitive, which matters here rather than being a nicety:
 * a server that answers JOIN #SNOS to a JOIN #snos would otherwise open a second
 * tab for the channel you are already in. */
static int ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (lowerc(*a) != lowerc(*b)) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static char *skipsp(char *s) {
  while (*s == ' ') s++;
  return s;
}

/* Terminate the word `s` points at and return whatever follows it, already
 * skipped past the separating spaces. Destructive, which is what makes parsing
 * a 512-byte IRC line cost no copies at all. */
static char *word(char *s) {
  while (*s && *s != ' ') s++;
  if (*s) {
    *s++ = 0;
    while (*s == ' ') s++;
  }
  return s;
}

/* The trailing parameter — everything after the " :" that IRC uses to mean "the
 * rest of the line, spaces included". Falls back to the whole string, so an
 * unknown numeric still shows something rather than nothing. */
static char *trail(char *s) {
  char *p = s;
  while (*p) {
    if (p[0] == ':' && (p == s || p[-1] == ' ')) return p + 1;
    p++;
  }
  return s;
}

/* ------------------------------------------------------------------- tabs */

static int tab_find(const char *name) {
  int i;
  for (i = 0; i < (int)ntabs; i++)
    if (ieq(tabname[i], name)) return i;
  return -1;
}

static int tab_get(const char *name) {
  int i = tab_find(name);
  if (i >= 0) return i;
  if (ntabs >= MAXTABS) return 0; /* out of tabs: fold into the status tab */
  i = (int)ntabs;
  ntabs++;
  scpy(tabname[i], name, TABNAME_MAX + 1);
  lcount[i] = 0;
  lhead[i] = 0;
  tact[i] = 0;
  nusers[i] = 0;
  bar_dirty = 1;
  return i;
}

static void tab_close(int t) {
  int i, j;
  if (t <= 0 || t >= (int)ntabs) return; /* the status tab cannot be closed */
  for (i = t; i + 1 < (int)ntabs; i++) {
    scpy(tabname[i], tabname[i + 1], TABNAME_MAX + 1);
    for (j = 0; j < TABLINES; j++) scpy(lines[i][j], lines[i + 1][j], LINE_W + 1);
    lcount[i] = lcount[i + 1];
    lhead[i] = lhead[i + 1];
    tact[i] = tact[i + 1];
    nusers[i] = nusers[i + 1];
    for (j = 0; j < USERS_MAX; j++) scpy(users[i][j], users[i + 1][j], USER_LEN + 1);
  }
  ntabs--;
  if (cur >= ntabs) cur = (uint8_t)(ntabs - 1);
  bar_dirty = 1;
  pane_force = 1;
}

/* ------------------------------------------------------------- user lists */

static void user_add(int t, const char *n) {
  int i;
  if (t < 0 || t >= (int)ntabs || n[0] == 0) return;
  for (i = 0; i < (int)nusers[t]; i++)
    if (ieq(users[t][i], n)) return;
  if (nusers[t] >= USERS_MAX) return;
  scpy(users[t][nusers[t]], n, USER_LEN + 1);
  nusers[t]++;
}

static void user_del(int t, const char *n) {
  int i, j;
  if (t < 0 || t >= (int)ntabs) return;
  for (i = 0; i < (int)nusers[t]; i++) {
    if (ieq(users[t][i], n)) {
      for (j = i; j + 1 < (int)nusers[t]; j++)
        scpy(users[t][j], users[t][j + 1], USER_LEN + 1);
      nusers[t]--;
      return;
    }
  }
}

static void user_del_all(const char *n) {
  int t;
  for (t = 0; t < (int)ntabs; t++) user_del(t, n);
}

static void user_rename(const char *from, const char *to) {
  int t, i;
  for (t = 0; t < (int)ntabs; t++)
    for (i = 0; i < (int)nusers[t]; i++)
      if (ieq(users[t][i], from)) scpy(users[t][i], to, USER_LEN + 1);
}

/* ------------------------------------------------------------ line buffer */

static void push_raw(int t, const char *s) {
  uint8_t h;
  if (t < 0 || t >= (int)ntabs) t = 0;
  h = lhead[t];
  scpy(lines[t][h], s, LINE_W + 1);
  lhead[t] = (uint8_t)((h + 1) % TABLINES);
  if (lcount[t] < TABLINES) lcount[t]++;
  if (t == (int)cur) {
    if (!pane_dirty) pane_since = sys_frames();
    pane_dirty = 1;
    /* Hold the view still while somebody is reading back through it. Scrolling
     * the text out from under a reader because a new line arrived is the one
     * thing scrollback must not do. */
    if (scroll) scroll++;
  } else if (!tact[t]) {
    tact[t] = 1;
    bar_dirty = 1;
  }
}

/* Word-wrap at 64 columns. A hard break mid-word is legible; a hard break every
 * time is not, so a space in the second half of the row is preferred. */
static void emit(int t, const char *s) {
  uint16_t n = slen(s), start = 0;
  if (n == 0) {
    push_raw(t, "");
    return;
  }
  while (start < n) {
    uint16_t left = (uint16_t)(n - start), take, i, br = 0;
    if (left <= LINE_W) {
      take = left;
    } else {
      for (i = 0; i <= LINE_W; i++)
        if (s[start + i] == ' ') br = i;
      take = (br > LINE_W / 2) ? br : LINE_W;
    }
    for (i = 0; i < take; i++) wrapbuf[i] = s[start + i];
    wrapbuf[take] = 0;
    push_raw(t, wrapbuf);
    start = (uint16_t)(start + take);
    while (start < n && s[start] == ' ') start++;
  }
}

/* Compose up to four fragments and emit them. `msg` is the only scratch, so no
 * caller may pass a pointer into it. */
static void emitv(int t, const char *a, const char *b, const char *c,
                  const char *d) {
  msg[0] = 0;
  if (a) scat(msg, a, sizeof msg);
  if (b) scat(msg, b, sizeof msg);
  if (c) scat(msg, c, sizeof msg);
  if (d) scat(msg, d, sizeof msg);
  emit(t, msg);
}

/* --------------------------------------------------------------- painting */

static void pad_row(char *b) {
  uint16_t i = slen(b);
  while (i < LINE_W) b[i++] = ' ';
  b[LINE_W] = 0;
}

static void paint_bar(void) {
  /* The right-hand end says who you are, or that you are not anybody yet. That
   * is the one piece of state nothing else on screen can tell you — a 433 that
   * renamed you happens between other traffic and is easy to miss. */
  /* Being scrolled back displaces the nick rather than sitting beside it: it is
   * a mode, and a mode you cannot see is one you get stuck in. */
  const char *right = scroll ? "^ scrollback"
                            : (registered ? nick
                                          : (disconnected ? "offline" : "..."));
  uint16_t rl = slen(right);
  uint16_t at = (uint16_t)(LINE_W - rl);
  uint16_t limit = (at > 1u) ? (uint16_t)(at - 1u) : 0u; /* keep one blank */
  uint16_t p = 0, k;
  int i;

  /* Solid inverse video with the ACTIVE tab punched out of it in normal video.
   * Mode 5 spends both BG layers on text columns, so inverse glyphs are the
   * only highlight the hardware has to offer. */
  for (i = 0; i < (int)ntabs && p < limit; i++) {
    uint8_t inv = (i == (int)cur) ? 0x00 : 0x80;
    const char *nm = tabname[i];
    if (p && p < limit) pbuf[p++] = (char)(' ' | 0x80);
    if (p < limit) pbuf[p++] = (char)(('1' + i) | inv);
    for (k = 0; nm[k] && p < limit; k++) pbuf[p++] = (char)((uint8_t)nm[k] | inv);
    if (tact[i] && i != (int)cur && p < limit) pbuf[p++] = (char)('*' | 0x80);
  }
  while (p < LINE_W) pbuf[p++] = (char)(' ' | 0x80);
  for (k = 0; k < rl; k++) pbuf[at + k] = (char)((uint8_t)right[k] | 0x80);
  pbuf[LINE_W] = 0;
  sys_puts_at(0, 0, pbuf);
  bar_dirty = 0;
}

static void paint_pane_rows(void) {
  int ph = (int)rows - 2;
  int n = (int)lcount[cur];
  int back = (int)scroll, max = n - ph, first, r;
  /* Clamped here rather than at the key, because the limit moves: lines arrive,
   * tabs change, and the on-screen keyboard changes the pane's height under us. */
  if (max < 0) max = 0;
  if (back > max) back = max;
  scroll = (uint16_t)back;
  first = n - ph - back;
  if (first < 0) first = 0;
  for (r = 0; r < ph; r++) {
    int idx = first + r;
    if (idx < n) {
      int slot = ((int)lhead[cur] - n + idx + TABLINES * 2) % TABLINES;
      scpy(pbuf, lines[cur][slot], LINE_W + 1);
    } else {
      pbuf[0] = 0;
    }
    pad_row(pbuf);
    sys_puts_at(r + 1, 0, pbuf);
  }
}

static void paint_overlay_tabs(void) {
  int ph = (int)rows - 2, r;
  for (r = 0; r < ph; r++) {
    pbuf[0] = 0;
    if (r == 0) {
      scpy(pbuf, " windows - digit or up/down, enter picks, esc closes",
           LINE_W + 1);
    } else if (r - 1 < (int)ntabs) {
      int i = r - 1;
      uint16_t p = 0;
      uint8_t inv = (i == (int)ovsel) ? 0x80 : 0x00;
      const char *nm = tabname[i];
      uint16_t k;
      pbuf[p++] = ' ';
      pbuf[p++] = (char)(('1' + i) | inv);
      pbuf[p++] = (char)(' ' | inv);
      for (k = 0; nm[k] && p + 1 < LINE_W; k++)
        pbuf[p++] = (char)((uint8_t)nm[k] | inv);
      if (tact[i]) {
        const char *s = "   (new)";
        for (k = 0; s[k] && p + 1 < LINE_W; k++) pbuf[p++] = s[k];
      }
      pbuf[p] = 0;
    }
    pad_row(pbuf);
    sys_puts_at(r + 1, 0, pbuf);
  }
}

static void paint_overlay_users(void) {
  int ph = (int)rows - 2, r;
  int n = (int)nusers[cur];
  int perrow = 3;
  for (r = 0; r < ph; r++) {
    pbuf[0] = 0;
    if (r == 0) {
      msg[0] = 0;
      scat(msg, " users in ", sizeof msg);
      scat(msg, tabname[cur], sizeof msg);
      scat(msg, " - esc or ^U closes", sizeof msg);
      scpy(pbuf, msg, LINE_W + 1);
    } else {
      int base = (int)ovtop + (r - 1) * perrow, k;
      uint16_t p = 0;
      for (k = 0; k < perrow; k++) {
        int idx = base + k;
        uint16_t j;
        uint16_t col = (uint16_t)(1 + k * 21);
        if (idx >= n) break;
        while (p < col && p + 1 < LINE_W) pbuf[p++] = ' ';
        for (j = 0; users[cur][idx][j] && p + 1 < LINE_W; j++)
          pbuf[p++] = users[cur][idx][j];
      }
      pbuf[p] = 0;
    }
    pad_row(pbuf);
    sys_puts_at(r + 1, 0, pbuf);
  }
}

static void paint_pane(void) {
  if (overlay == OV_TABS) paint_overlay_tabs();
  else if (overlay == OV_USERS) paint_overlay_users();
  else paint_pane_rows();
  pane_dirty = 0;
  pane_force = 0;
}

static void paint_input(void) {
  uint16_t avail = LINE_W - 3, start = 0, p = 0, i;
  if (inn > avail) start = (uint16_t)(inn - avail);
  pbuf[p++] = '>';
  pbuf[p++] = ' ';
  for (i = start; i < inn && p + 1 < LINE_W; i++) pbuf[p++] = input[i];
  if (p < LINE_W) pbuf[p++] = (char)(' ' | 0x80); /* the caret */
  while (p < LINE_W) pbuf[p++] = ' ';
  pbuf[LINE_W] = 0;
  sys_puts_at((int)rows - 1, 0, pbuf);
  input_dirty = 0;
}

static void repaint(int busy) {
  uint16_t now = sys_frames();
  if (bar_dirty) paint_bar();
  if (input_dirty) paint_input();
  if (!(pane_dirty || pane_force)) return;
  if (!pane_force) {
    if (busy) {
      if ((uint16_t)(now - pane_since) < PANE_MAX_DEFER) return;
    } else if ((uint16_t)(now - last_pane) < PANE_MIN_FRAMES) {
      return;
    }
  }
  paint_pane();
  last_pane = now;
}

/* --------------------------------------------------------------- sending */

static int send_all(const char *p, uint16_t n) { return sl_send_all(ch, p, n); }

/* Every outgoing line goes through here, so the CRLF is in exactly one place.
 * No caller may pass a pointer into `outbuf`. */
static int sendv(const char *a, const char *b, const char *c, const char *d) {
  outbuf[0] = 0;
  if (a) scat(outbuf, a, sizeof outbuf);
  if (b) scat(outbuf, b, sizeof outbuf);
  if (c) scat(outbuf, c, sizeof outbuf);
  if (d) scat(outbuf, d, sizeof outbuf);
  scat(outbuf, "\r\n", sizeof outbuf);
  return send_all(outbuf, slen(outbuf));
}

/* -------------------------------------------------------------- protocol */

static void names_reply(char *rest) {
  /* "<ournick> = #chan :@alice bob" — symbol, channel, then the list. */
  char *sym = skipsp(rest);
  char *r2 = word(sym);
  char *chan = skipsp(r2);
  char *r3 = word(chan);
  char *names = skipsp(r3);
  int t = tab_find(chan);
  if (t < 0) return;
  if (*names == ':') names++;
  while (*names) {
    char *n = skipsp(names);
    char *next;
    if (*n == 0) break;
    next = word(n);
    /* Channel-status prefixes. They are display sugar and change on a MODE we
     * would have to track, so they are dropped rather than shown stale. */
    while (*n == '@' || *n == '+' || *n == '%' || *n == '~' || *n == '&') n++;
    user_add(t, n);
    names = next;
  }
}

static void handle_numeric(uint16_t num, char *rest) {
  char *after = word(skipsp(rest)); /* drop the copy of our own nick */

  if (num == 1) {
    registered = 1;
    bar_dirty = 1;
    emit(0, trail(after));
    if (autojoin[0]) sendv("JOIN ", autojoin, 0, 0);
    return;
  }
  if (num == 433 || num == 432) {
    /* Nick taken (or invalid). Extend and retry, but only a few times: a server
     * that refuses every variant would otherwise have us in a send loop. */
    uint16_t l = slen(nick);
    if (nick_tries < 3 && l < NICK_MAX) {
      nick_tries++;
      nick[l] = '_';
      nick[l + 1] = 0;
      emitv(0, "* nick in use, trying ", nick, 0, 0);
      bar_dirty = 1;
      sendv("NICK ", nick, 0, 0);
    } else {
      emit(0, "* nick refused - use /nick");
    }
    return;
  }
  if (num == 353) {
    names_reply(after);
    return;
  }
  if (num == 366) {
    char *chan = skipsp(after);
    int t;
    word(chan);
    t = tab_find(chan);
    if (t >= 0) {
      char n[8];
      uint16_t v = nusers[t], i = 0, j;
      if (v == 0) {
        n[i++] = '0';
      } else {
        char d[6];
        j = 0;
        while (v && j < 6) {
          d[j++] = (char)('0' + (v % 10u));
          v /= 10u;
        }
        while (j) n[i++] = d[--j];
      }
      n[i] = 0;
      emitv(t, "* ", n, " users here (^U lists them)", 0);
    }
    return;
  }
  if (num == 332) { /* RPL_TOPIC */
    char *chan = skipsp(after);
    char *r2 = word(chan);
    int t = tab_find(chan);
    emitv(t < 0 ? 0 : t, "* topic: ", trail(r2), 0, 0);
    return;
  }
  emit(0, trail(after));
}

static void handle_line(char *s) {
  char *cmd, *rest;

  pfxnick[0] = 0;
  s = skipsp(s);
  if (*s == 0) return;

  if (*s == ':') {
    char *pre = s + 1;
    uint16_t i = 0;
    s = word(pre);
    while (pre[i] && pre[i] != '!' && pre[i] != '@' && i < NICK_MAX) {
      pfxnick[i] = pre[i];
      i++;
    }
    pfxnick[i] = 0;
  }

  cmd = skipsp(s);
  rest = word(cmd);
  if (*cmd == 0) return;

  /* First, unconditionally, before anything below can return early. */
  if (ieq(cmd, "PING")) {
    sendv("PONG ", (*rest ? rest : ":snos"), 0, 0);
    return;
  }
  if (ieq(cmd, "PONG")) return;

  if (ieq(cmd, "ERROR")) {
    emitv(0, "* server: ", trail(rest), 0, 0);
    return;
  }

  if (ieq(cmd, "PRIVMSG") || ieq(cmd, "NOTICE")) {
    char *target = skipsp(rest);
    char *text = word(target);
    int notice = ieq(cmd, "NOTICE");
    int t;
    if (*text == ':') text++;
    /* CTCP. OFTC sends a VERSION request unbidden on connect and there is
     * nothing useful to show for it. */
    if ((uint8_t)text[0] == 0x01) return;
    if (target[0] == '#' || target[0] == '&') t = tab_get(target);
    else if (notice || pfxnick[0] == 0) t = 0;
    else t = tab_get(pfxnick);
    if (notice) emitv(t, "-", pfxnick, "- ", text);
    else emitv(t, "<", pfxnick, "> ", text);
    return;
  }

  if (ieq(cmd, "JOIN")) {
    char *chan = skipsp(rest);
    if (*chan == ':') chan++;
    word(chan);
    if (ieq(pfxnick, nick)) {
      int t = tab_get(chan);
      cur = (uint8_t)t;
      tact[t] = 0;
      scroll = 0;
      bar_dirty = 1;
      pane_force = 1;
      emitv(t, "* now talking in ", chan, 0, 0);
    } else {
      int t = tab_find(chan);
      if (t < 0) t = 0;
      user_add(t, pfxnick);
      emitv(t, "* ", pfxnick, " has joined", 0);
    }
    return;
  }

  if (ieq(cmd, "PART")) {
    char *chan = skipsp(rest);
    int t;
    if (*chan == ':') chan++;
    word(chan);
    t = tab_find(chan);
    if (ieq(pfxnick, nick)) {
      if (t > 0) tab_close(t);
    } else {
      if (t < 0) t = 0;
      user_del(t, pfxnick);
      emitv(t, "* ", pfxnick, " has left", 0);
    }
    return;
  }

  if (ieq(cmd, "QUIT")) {
    /* No channel is named — QUIT is the one membership message that leaves
     * "where to show it" to the client. The per-tab user lists ARE the
     * who-is-where tracking (NAMES fills them, JOIN/PART/KICK keep them
     * current), so route the notice to every channel the user was actually
     * in, plus any query tab bearing their name; the status tab only hears
     * about users nobody shares a tab with. This handler predates the user
     * lists and used to claim routing was impossible. */
    char *reason = trail(rest);
    int t, shown = 0;
    /* Most servers send client quits as "Quit: <message>"; with our own
     * "quit:" label in front, the double prefix is pure noise. */
    if (reason[0] == 'Q' && reason[1] == 'u' && reason[2] == 'i' &&
        reason[3] == 't' && reason[4] == ':' && reason[5] == ' ')
      reason += 6;
    for (t = 1; t < (int)ntabs; t++) {
      int i, present = ieq(tabname[t], pfxnick);
      for (i = 0; !present && i < (int)nusers[t]; i++)
        if (ieq(users[t][i], pfxnick)) present = 1;
      if (present) {
        emitv(t, "* ", pfxnick, " quit: ", reason);
        shown = 1;
      }
    }
    user_del_all(pfxnick);
    if (!shown) emitv(0, "* ", pfxnick, " quit: ", reason);
    return;
  }

  if (ieq(cmd, "NICK")) {
    char *nn = skipsp(rest);
    if (*nn == ':') nn++;
    word(nn);
    if (ieq(pfxnick, nick)) {
      scpy(nick, nn, NICK_MAX + 1);
      bar_dirty = 1;
    }
    user_rename(pfxnick, nn);
    emitv(0, "* ", pfxnick, " is now ", nn);
    return;
  }

  if (ieq(cmd, "KICK")) {
    char *chan = skipsp(rest);
    char *r2 = word(chan);
    char *who = skipsp(r2);
    int t;
    word(who);
    t = tab_find(chan);
    /* Being kicked is PART's self-case, not a spectator line: leaving the tab
     * open kept a dead conversation on screen where every line went out as a
     * PRIVMSG the server refused with a 442 nobody sees. */
    if (ieq(who, nick)) {
      emitv(0, "* kicked from ", chan, " by ", pfxnick);
      if (t > 0) tab_close(t);
      return;
    }
    if (t < 0) t = 0;
    user_del(t, who);
    emitv(t, "* ", who, " was kicked by ", pfxnick);
    return;
  }

  if (ieq(cmd, "TOPIC") || ieq(cmd, "MODE")) {
    char *target = skipsp(rest);
    char *r2 = word(target);
    int t = tab_find(target);
    if (t < 0) t = 0;
    emitv(t, "* ", pfxnick, ieq(cmd, "TOPIC") ? " set topic: " : " sets mode: ",
          trail(r2));
    return;
  }

  if (cmd[0] >= '0' && cmd[0] <= '9' && slen(cmd) == 3) {
    handle_numeric((uint16_t)((cmd[0] - '0') * 100 + (cmd[1] - '0') * 10 +
                              (cmd[2] - '0')),
                   rest);
    return;
  }

  emitv(0, "* ", cmd, " ", trail(rest));
}

static void rx_byte(uint8_t c) {
  if (c == '\r') return;
  if (c == '\n') {
    rx[rxn] = 0;
    handle_line(rx);
    rxn = 0;
    return;
  }
  if (rxn < RX_MAX) rx[rxn++] = (char)c;
}

/* ------------------------------------------------------------- user input */

static void do_quit(const char *reason) {
  if (quitting) return;
  sendv("QUIT :", (reason && reason[0]) ? reason : "SNOS", 0, 0);
  quitting = 1;
}

static void say_to_current(const char *text) {
  if (cur == 0) {
    emit(0, "* no channel here - /join #something first");
    return;
  }
  sendv("PRIVMSG ", tabname[cur], " :", text);
  emitv((int)cur, "<", nick, "> ", text);
}

static void do_command(char *line) {
  char *cmd, *rest;

  if (line[0] != '/') {
    say_to_current(line);
    return;
  }
  cmd = line + 1;
  rest = word(cmd);

  if (ieq(cmd, "join") || ieq(cmd, "j")) {
    char *chan = skipsp(rest);
    if (*chan == 0) {
      emit((int)cur, "* usage: /join #channel");
      return;
    }
    /* Sent verbatim; the tab is opened by the server's JOIN echo, so a channel
     * that is invite-only or full never leaves an empty tab behind. */
    sendv("JOIN ", chan, 0, 0);
    return;
  }
  if (ieq(cmd, "part") || ieq(cmd, "leave")) {
    char *chan = skipsp(rest);
    if (*chan == 0) chan = tabname[cur];
    if (chan[0] != '#' && chan[0] != '&') {
      if (cur > 0) tab_close((int)cur);
      return;
    }
    sendv("PART ", chan, 0, 0);
    return;
  }
  if (ieq(cmd, "nick")) {
    char *nn = skipsp(rest);
    if (*nn) {
      word(nn);
      sendv("NICK ", nn, 0, 0);
    }
    return;
  }
  if (ieq(cmd, "msg") || ieq(cmd, "query")) {
    char *who = skipsp(rest);
    char *text = word(who);
    int t;
    if (*who == 0) {
      emit((int)cur, "* usage: /msg nick text");
      return;
    }
    t = tab_get(who);
    if (*text) {
      sendv("PRIVMSG ", who, " :", text);
      emitv(t, "<", nick, "> ", text);
    }
    cur = (uint8_t)t;
    tact[t] = 0;
    scroll = 0;
    bar_dirty = 1;
    pane_force = 1;
    return;
  }
  if (ieq(cmd, "names")) {
    if (cur > 0) sendv("NAMES ", tabname[cur], 0, 0);
    return;
  }
  if (ieq(cmd, "close") || ieq(cmd, "wc")) {
    if (cur > 0) {
      if (tabname[cur][0] == '#' || tabname[cur][0] == '&')
        sendv("PART ", tabname[cur], 0, 0);
      tab_close((int)cur);
    }
    return;
  }
  if (ieq(cmd, "raw") || ieq(cmd, "quote")) {
    if (*rest) sendv(rest, 0, 0, 0);
    return;
  }
  if (ieq(cmd, "quit") || ieq(cmd, "exit")) {
    do_quit(rest);
    return;
  }
  if (ieq(cmd, "help")) {
    emit((int)cur, "/join #c  /part  /msg nick text  /nick n  /names");
    emit((int)cur, "/close  /raw LINE  /quit [reason]");
    emit((int)cur, "^N ^P switch  TAB windows  ^U users  ^Q quit");
    emit((int)cur, "up/down scrolls back through this window");
    return;
  }
  emitv((int)cur, "* unknown command: /", cmd, 0, 0);
}

/* Half a screen per press. A line at a time is precise and tedious; a whole
 * screen leaves no overlap to read across. */
static void pane_scroll(int dir) {
  int ph = (int)rows - 2;
  int n = (int)lcount[cur];
  int max = n - ph;
  int back = (int)scroll + dir * (ph / 2);
  if (max < 0) max = 0;
  if (back > max) back = max;
  if (back < 0) back = 0;
  if ((uint16_t)back == scroll) return;
  scroll = (uint16_t)back;
  bar_dirty = 1;
  pane_force = 1;
}

static void switch_tab(int d) {
  int t = (int)cur + d;
  if (ntabs == 0) return;
  while (t < 0) t += (int)ntabs;
  t %= (int)ntabs;
  cur = (uint8_t)t;
  tact[t] = 0;
  scroll = 0; /* each window is entered live; the old position meant nothing here */
  bar_dirty = 1;
  pane_force = 1;
}

static void overlay_key(uint16_t k) {
  if (k == KEY_ESC || k == KEY_TABS || k == KEY_USERS) {
    overlay = OV_NONE;
    pane_force = 1;
    return;
  }
  if (overlay == OV_TABS) {
    if (k >= '1' && k < (uint16_t)('1' + ntabs)) {
      cur = (uint8_t)(k - '1');
      tact[cur] = 0;
      scroll = 0;
      overlay = OV_NONE;
      bar_dirty = 1;
      pane_force = 1;
      return;
    }
    if (k == SNOS_KEY_UP) {
      if (ovsel) ovsel--;
      pane_force = 1;
      return;
    }
    if (k == SNOS_KEY_DOWN) {
      if (ovsel + 1 < ntabs) ovsel++;
      pane_force = 1;
      return;
    }
    if (k == '\n' || k == '\r') {
      cur = ovsel;
      tact[cur] = 0;
      scroll = 0;
      overlay = OV_NONE;
      bar_dirty = 1;
      pane_force = 1;
      return;
    }
    return;
  }
  /* OV_USERS: a long list scrolls three across per row. */
  if (k == SNOS_KEY_DOWN) {
    if ((uint16_t)ovtop + 3u < (uint16_t)nusers[cur]) ovtop = (uint8_t)(ovtop + 3);
    pane_force = 1;
    return;
  }
  if (k == SNOS_KEY_UP) {
    ovtop = (uint8_t)(ovtop >= 3 ? ovtop - 3 : 0);
    pane_force = 1;
    return;
  }
}

static void on_key(uint16_t k) {
  if (overlay != OV_NONE) {
    overlay_key(k);
    return;
  }
  if (k == '\n' || k == '\r') {
    if (inn == 0) return;
    input[inn] = 0;
    scpy(cmdbuf, input, sizeof cmdbuf);
    inn = 0;
    input[0] = 0;
    input_dirty = 1;
    do_command(cmdbuf);
    return;
  }
  if (k == '\b') {
    if (inn) {
      inn--;
      input[inn] = 0;
      input_dirty = 1;
    }
    return;
  }
  if (k == KEY_NEXT || k == SNOS_KEY_RIGHT) {
    switch_tab(1);
    return;
  }
  if (k == KEY_PREV || k == SNOS_KEY_LEFT) {
    switch_tab(-1);
    return;
  }
  if (k == SNOS_KEY_UP) {
    pane_scroll(1);
    return;
  }
  if (k == SNOS_KEY_DOWN) {
    pane_scroll(-1);
    return;
  }
  if (k == KEY_TABS) {
    overlay = OV_TABS;
    ovsel = cur;
    pane_force = 1;
    return;
  }
  if (k == KEY_USERS) {
    overlay = OV_USERS;
    ovtop = 0;
    pane_force = 1;
    return;
  }
  if (k == KEY_QUIT) {
    do_quit("SNOS");
    return;
  }
  if (k >= 0x20 && k <= 0x7E) {
    if (inn < INPUT_MAX) {
      input[inn++] = (char)k;
      input[inn] = 0;
      input_dirty = 1;
    }
    return;
  }
}

/* ------------------------------------------------------------------- main */

static void read_screen(void) {
  uint16_t sc = sys_screen();
  uint8_t r = (uint8_t)(sc >> 8), c = (uint8_t)(sc & 0xFFu);
  if (r < 5) r = 5;
  if (r != rows || c != cols) {
    rows = r;
    cols = c;
    bar_dirty = 1;
    input_dirty = 1;
    pane_force = 1;
  }
}

int main(void) {
  char *hostarg, *nickarg, *chanarg;
  uint16_t st, deadline;

  /* Initialised statics do NOT survive in an application: --no-auto-libraries
   * means the data-init table is never walked, so every initialiser in this
   * file would read back as zero. Everything that needs a value gets it here.
   * See docs/calypsi-asm-syntax.md. */
  rxn = 0;
  inn = 0;
  ntabs = 0;
  cur = 0;
  overlay = OV_NONE;
  ovsel = 0;
  ovtop = 0;
  scroll = 0;
  quitting = 0;
  registered = 0;
  disconnected = 0;
  nick_tries = 0;
  bar_dirty = 1;
  pane_dirty = 1;
  input_dirty = 1;
  pane_force = 1;
  last_pane = 0;
  rows = 28;
  cols = 64;
  input[0] = 0;
  autojoin[0] = 0;

  snos_getargs(args, sizeof args);
  hostarg = skipsp(args);
  nickarg = word(hostarg);
  chanarg = word(nickarg);
  word(chanarg);

  if (*hostarg == 0) {
    sys_puts("usage: irc HOST[:PORT] [NICK] [#CHANNEL]\n");
    sys_puts("       port defaults to 6667, nick to 'snes'\n");
    return 1;
  }

  /* `irc host #chan` is the obvious thing to type when you are happy with the
   * default nick, and reading it positionally would connect you as "#chan". A
   * channel is unmistakable, so take it as one wherever it appears. */
  if (nickarg[0] == '#' || nickarg[0] == '&') {
    chanarg = nickarg;
    nickarg = chanarg + slen(chanarg); /* the empty string at its end */
  }
  scpy(nick, (*nickarg ? nickarg : "snes"), NICK_MAX + 1);
  if (*chanarg) scpy(autojoin, chanarg, TABNAME_MAX + 1);
  sl_hostport(hostport, sizeof hostport, hostarg, ":6667");

  if (!sys_net_up()) {
    sys_puts("no gateway attached\n");
    return 1;
  }

  read_screen();
  sys_cls();
  tab_get("*");
  emitv(0, "* SNOS irc - connecting to ", hostport, " as ", nick);
  emit(0, "* /help lists the commands");
  paint_bar();
  paint_pane();
  paint_input();

  ch = sys_net_connect(hostport);
  if (ch == SYS_ERR) {
    sys_cls();
    sys_puts("connect failed: no free channel\n");
    return 1;
  }

  /* sl_wait_open closes the channel on every non-OK outcome. */
  switch (sl_wait_open(ch, OPEN_DEADLINE, 1)) {
  case SL_OPEN_OK:
    break;
  case SL_OPEN_TIMEOUT:
    sys_cls();
    sys_puts("timeout connecting to ");
    sys_puts(hostport);
    sys_putc('\n');
    return 1;
  case SL_OPEN_CANCELLED:
    sys_cls();
    sys_puts("[cancelled]\n");
    return 1;
  default:
    sys_cls();
    sys_puts("could not connect to ");
    sys_puts(hostport);
    sys_putc('\n');
    return 1;
  }

  /* Sampled with the send queue known empty, so it is the queue's capacity —
   * which is what "everything I wrote has drained" is measured against on the
   * way out. */
  txmax = sys_net_wspace(ch);

  /* Register. No CAP negotiation on purpose — see the header comment. */
  emit(0, "* connected, registering");
  sendv("NICK ", nick, 0, 0);
  sendv("USER ", nick, " 0 * :SNOS IRC", 0);
  last_rx = sys_frames();

  for (;;) {
    uint16_t k;
    int busy = 0;

    /* Drain everything that has arrived before repainting once, and tell
     * repaint() whether more is still coming — see PANE_MAX_DEFER. */
    for (;;) {
      uint16_t n = sys_net_read(ch, nbuf, NET_BUF), i;
      if (n == 0 || n == SYS_ERR) break;
      busy = 1;
      for (i = 0; i < n; i++) rx_byte(nbuf[i]);
    }
    if (busy) {
      /* Bytes arrived: the pipe is proven, whatever they said. */
      last_rx = sys_frames();
      ping_sent = 0;
    }

    while ((k = sys_pollkey()) != SYS_ERR) {
      on_key(k);
      if (quitting) break;
    }
    if (quitting) break;

    st = sys_net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING && !disconnected) {
      disconnected = 1;
      emit(0, st == NET_ST_ERROR ? "* connection failed"
                                 : "* disconnected - ^Q to leave");
      cur = 0;
      scroll = 0;
      bar_dirty = 1;
      pane_force = 1;
    }

    /* The keepalive probe, and its verdict. */
    if (!disconnected) {
      if (!ping_sent) {
        if ((int16_t)(sys_frames() - (uint16_t)(last_rx + KEEPALIVE_FRAMES)) >= 0) {
          sendv("PING :snos", 0, 0, 0);
          ping_sent = 1;
          pong_by = (uint16_t)(sys_frames() + PONG_GRACE);
        }
      } else if ((int16_t)(sys_frames() - pong_by) >= 0) {
        disconnected = 1;
        emit(0, "* connection lost (ping timeout)");
        cur = 0;
        scroll = 0;
        bar_dirty = 1;
        pane_force = 1;
      }
    }

    read_screen();
    repaint(busy);
    sys_yield();
  }

  /* Let the QUIT actually reach the wire before letting go.
   *
   * NOT sys_net_hangup: that one only applies to a LISTENING channel — it puts
   * the port back into LISTEN and refuses outright on anything else
   * (kernel/net.c) — so a client connection would spin on it for nothing. What
   * a client needs is simply for its send queue to drain, because closing on top
   * of a queued QUIT truncates it and the server records a dropped connection
   * rather than a departure.
   *
   * txmax was sampled while the queue was known empty, so it is the ring's own
   * capacity without this file having to know what the kernel chose. */
  {
    uint16_t give_up = (uint16_t)(sys_frames() + 300u);
    while (sys_net_wspace(ch) < txmax) {
      uint16_t s2 = sys_net_state(ch);
      if (s2 != NET_ST_OPEN && s2 != NET_ST_EOF) break;
      if ((int16_t)(sys_frames() - give_up) >= 0) break;
      sys_yield();
    }
    /* And a beat more, for the last frame to be acknowledged rather than merely
     * dequeued: leaving the ring is not the same as crossing the link. */
    {
      uint16_t t = (uint16_t)(sys_frames() + 15u);
      while ((int16_t)(sys_frames() - t) < 0) sys_yield();
    }
  }
  sys_net_close(ch);
  sys_cls();
  sys_puts("irc: disconnected from ");
  sys_puts(hostport);
  sys_putc('\n');
  return 0;
}
