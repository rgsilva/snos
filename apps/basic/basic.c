/* SNOS BASIC — a small integer BASIC, written as a SNOS application.
 *
 * Why this rather than a port. PLAN.md M5 called for porting Stefan's BASIC
 * from ../tinybasic, and two things ruled it out on inspection:
 *
 *   - Licensing is ambiguous. The top-level LICENSE (2025) is BSD-3-Clause but
 *     every source file still carries a GPL-3 header comment. For a ROM that is
 *     meant to be freely redistributable that is not a question to guess at.
 *   - Fit. 8400 lines built on stdio/stdlib/time/dirent and a hardware layer
 *     spanning VGA, SPI, I2C, RTC and sensors — none of which exist here —
 *     plus a pile of `long` and `float` arithmetic to push through a 16-bit-int
 *     compiler at -O0.
 *
 * So this is purpose-built: 16-bit signed integers throughout (which is what
 * the 65816 and Calypsi both want anyway), no C library, and every side effect
 * through a SNOS system call. It is the first real application for this OS, and
 * it exercises the parts of the kernel that matter — console, line input, the
 * filesystem for SAVE and LOAD, and a clean exit back to the shell.
 *
 * Language: line numbers, LET/PRINT/IF-THEN/GOTO/GOSUB/RETURN/FOR-NEXT/INPUT/
 * REM/END/CLS, 26 integer variables A-Z, one array @(), and the usual operators
 * with precedence. Immediate commands: RUN LIST NEW SAVE LOAD FILES HELP,
 * and BYE / EXIT / QUIT to leave.
 */
#include "snos_abi.h"

#define MAXLINES 128
#define POOLSIZE 6144
#define ARRAYSIZE 64
#define GOSUB_DEPTH 12
#define FOR_DEPTH 8
#define LINEBUF 128

typedef int16_t num_t;

/* ---- program storage: line numbers plus a text pool ---------------------- */
static uint16_t ln_num[MAXLINES];
static uint16_t ln_off[MAXLINES];
static uint8_t ln_len[MAXLINES];
static char pool[POOLSIZE];
static uint16_t nlines, poolused;

/* ---- interpreter state -------------------------------------------------- */
static num_t vars[26];
static num_t array[ARRAYSIZE];
static char line[LINEBUF];

static const char *cur;   /* parse cursor */
static int pc;            /* index of the executing line, -1 = immediate */
static int running;
static int stop_flag;
static const char *errmsg;

static uint16_t gosub_line[GOSUB_DEPTH];
static uint16_t gosub_pos[GOSUB_DEPTH];
static int gosub_sp;

struct forrec {
  int var;
  num_t limit, step;
  int line;
  uint16_t pos;
};
static struct forrec fors[FOR_DEPTH];
static int for_sp;

/* Assigned in main(), NEVER here: apps link with --no-auto-libraries, so
 * file-scope initialisers are silently discarded and read as zero -- and a
 * zero xorshift state stays zero forever, making RND permanently 0. */
static uint16_t rnd_state;

/* ---- tiny helpers (no C library) ---------------------------------------- */

static void outc(char c) { sys_putc((uint16_t)(unsigned char)c); }
static void outs(const char *s) { sys_puts(s); }

static void outnum(num_t v) {
  uint16_t u;
  if (v < 0) {
    outc('-');
    u = (uint16_t)(-(int32_t)v);
  } else {
    u = (uint16_t)v;
  }
  sys_putu(u);
}

static void nl(void) { outc('\n'); }

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void skipsp(void) {
  while (*cur == ' ' || *cur == '\t') cur++;
}

/* Case-insensitive keyword match; consumes it and any trailing space. */
static int keyword(const char *kw) {
  const char *p = cur;
  while (*kw) {
    if (upper(*p) != *kw) return 0;
    p++;
    kw++;
  }
  if (is_alpha(*p)) return 0; /* do not match a prefix of a longer word */
  cur = p;
  skipsp();
  return 1;
}

static void err(const char *m) {
  if (errmsg == 0) errmsg = m;
}

/* ---- expressions -------------------------------------------------------- */

static num_t expr(void);

static num_t factor(void) {
  num_t v = 0;
  skipsp();
  if (*cur == '-') {
    cur++;
    return (num_t)(-factor());
  }
  if (*cur == '(') {
    cur++;
    v = expr();
    skipsp();
    if (*cur == ')') cur++;
    else err("missing )");
    return v;
  }
  if (is_digit(*cur)) {
    while (is_digit(*cur)) {
      v = (num_t)(v * 10 + (*cur - '0'));
      cur++;
    }
    return v;
  }
  if (*cur == '@') { /* the one array: @(index) */
    cur++;
    skipsp();
    if (*cur != '(') {
      err("@ needs (");
      return 0;
    }
    cur++;
    v = expr();
    skipsp();
    if (*cur == ')') cur++;
    if (v < 0 || v >= ARRAYSIZE) {
      err("index");
      return 0;
    }
    return array[v];
  }
  if (keyword("RND")) {
    skipsp();
    if (*cur == '(') {
      cur++;
      v = expr();
      skipsp();
      if (*cur == ')') cur++;
    }
    /* xorshift: deterministic, which is what a test wants */
    rnd_state ^= (uint16_t)(rnd_state << 7);
    rnd_state ^= (uint16_t)(rnd_state >> 9);
    rnd_state ^= (uint16_t)(rnd_state << 8);
    if (v <= 0) return (num_t)(rnd_state & 0x7FFF);
    return (num_t)((rnd_state & 0x7FFF) % (uint16_t)v);
  }
  if (keyword("ABS")) {
    skipsp();
    if (*cur == '(') cur++;
    v = expr();
    skipsp();
    if (*cur == ')') cur++;
    return v < 0 ? (num_t)(-v) : v;
  }
  if (is_alpha(*cur)) {
    int idx = upper(*cur) - 'A';
    cur++;
    if (is_alpha(*cur) || is_digit(*cur)) {
      err("bad name"); /* variables are single letters */
      return 0;
    }
    return vars[idx];
  }
  err("syntax");
  return 0;
}

static num_t term(void) {
  num_t v = factor();
  for (;;) {
    skipsp();
    if (*cur == '*') {
      cur++;
      v = (num_t)(v * factor());
    } else if (*cur == '/') {
      num_t d;
      cur++;
      d = factor();
      if (d == 0) {
        err("divide by zero");
        return 0;
      }
      v = (num_t)(v / d);
    } else if (*cur == '%') {
      num_t d;
      cur++;
      d = factor();
      if (d == 0) {
        err("divide by zero");
        return 0;
      }
      v = (num_t)(v % d);
    } else {
      return v;
    }
  }
}

static num_t sum(void) {
  num_t v = term();
  for (;;) {
    skipsp();
    if (*cur == '+') {
      cur++;
      v = (num_t)(v + term());
    } else if (*cur == '-') {
      cur++;
      v = (num_t)(v - term());
    } else {
      return v;
    }
  }
}

static num_t expr(void) {
  num_t a = sum();
  skipsp();
  if (*cur == '=') {
    cur++;
    return (num_t)(a == sum());
  }
  if (*cur == '<' && cur[1] == '>') {
    cur += 2;
    return (num_t)(a != sum());
  }
  if (*cur == '<' && cur[1] == '=') {
    cur += 2;
    return (num_t)(a <= sum());
  }
  if (*cur == '>' && cur[1] == '=') {
    cur += 2;
    return (num_t)(a >= sum());
  }
  if (*cur == '<') {
    cur++;
    return (num_t)(a < sum());
  }
  if (*cur == '>') {
    cur++;
    return (num_t)(a > sum());
  }
  return a;
}

/* ---- program editing ---------------------------------------------------- */

static int find_line(uint16_t n) {
  int i;
  for (i = 0; i < (int)nlines; i++)
    if (ln_num[i] == n) return i;
  return -1;
}

static void pool_remove(uint16_t off, uint16_t len) {
  uint16_t i;
  for (i = off; i + len < poolused; i++) pool[i] = pool[i + len];
  poolused = (uint16_t)(poolused - len);
  for (i = 0; i < nlines; i++)
    if (ln_off[i] > off) ln_off[i] = (uint16_t)(ln_off[i] - len);
}

static void delete_line(uint16_t n) {
  int i = find_line(n), j;
  if (i < 0) return;
  pool_remove(ln_off[i], ln_len[i]);
  for (j = i; j < (int)nlines - 1; j++) {
    ln_num[j] = ln_num[j + 1];
    ln_off[j] = ln_off[j + 1];
    ln_len[j] = ln_len[j + 1];
  }
  nlines--;
}

static void insert_line(uint16_t n, const char *text) {
  uint16_t len = 0, off;
  int i, j;
  while (text[len]) len++;

  delete_line(n);
  if (len == 0) return; /* bare line number deletes */
  if (nlines >= MAXLINES) {
    outs("too many lines\n");
    return;
  }
  if (poolused + len + 1 > POOLSIZE) {
    outs("program too big\n");
    return;
  }

  off = poolused;
  for (i = 0; i < (int)len; i++) pool[off + i] = text[i];
  pool[off + len] = 0;
  poolused = (uint16_t)(poolused + len + 1);

  for (i = 0; i < (int)nlines; i++)
    if (ln_num[i] > n) break;
  for (j = (int)nlines; j > i; j--) {
    ln_num[j] = ln_num[j - 1];
    ln_off[j] = ln_off[j - 1];
    ln_len[j] = ln_len[j - 1];
  }
  ln_num[i] = n;
  ln_off[i] = off;
  ln_len[i] = (uint8_t)(len + 1);
  nlines++;
}

static void cmd_new(void) {
  nlines = 0;
  poolused = 0;
}

static void cmd_list(void) {
  int i;
  for (i = 0; i < (int)nlines; i++) {
    outnum((num_t)ln_num[i]);
    outc(' ');
    outs(&pool[ln_off[i]]);
    nl();
  }
}

/* ---- statements --------------------------------------------------------- */

static void do_print(void) {
  int printed_nl = 1;
  for (;;) {
    skipsp();
    if (*cur == 0 || *cur == ':') break;
    if (*cur == '"') {
      cur++;
      while (*cur && *cur != '"') outc(*cur++);
      if (*cur == '"') cur++;
    } else {
      outnum(expr());
      if (errmsg) return;
    }
    skipsp();
    if (*cur == ';') {
      cur++;
      printed_nl = 0;
      continue;
    }
    if (*cur == ',') {
      cur++;
      outc(' ');
      printed_nl = 0;
      continue;
    }
    printed_nl = 1;
    break;
  }
  if (printed_nl) nl();
}

static void do_input(void) {
  int idx;
  const char *save;
  skipsp();
  if (*cur == '"') {
    cur++;
    while (*cur && *cur != '"') outc(*cur++);
    if (*cur == '"') cur++;
    skipsp();
    if (*cur == ';' || *cur == ',') cur++;
    skipsp();
  } else {
    outs("? ");
  }
  if (!is_alpha(*cur)) {
    err("INPUT needs a variable");
    return;
  }
  idx = upper(*cur) - 'A';
  cur++;

  save = cur;
  snos_set_ptr(line);
  sys_readline_raw(LINEBUF);
  {
    const char *p = line;
    num_t v = 0;
    int neg = 0;
    while (*p == ' ') p++;
    if (*p == '-') {
      neg = 1;
      p++;
    }
    while (is_digit(*p)) {
      v = (num_t)(v * 10 + (*p - '0'));
      p++;
    }
    vars[idx] = neg ? (num_t)(-v) : v;
  }
  cur = save;
}

static void do_assign(void) {
  int idx;
  skipsp();
  if (*cur == '@') {
    num_t i;
    cur++;
    skipsp();
    if (*cur != '(') {
      err("@ needs (");
      return;
    }
    cur++;
    i = expr();
    skipsp();
    if (*cur == ')') cur++;
    skipsp();
    if (*cur != '=') {
      err("expected =");
      return;
    }
    cur++;
    if (i < 0 || i >= ARRAYSIZE) {
      err("index");
      return;
    }
    array[i] = expr();
    return;
  }
  if (!is_alpha(*cur)) {
    err("syntax");
    return;
  }
  idx = upper(*cur) - 'A';
  cur++;
  skipsp();
  if (*cur != '=') {
    err("expected =");
    return;
  }
  cur++;
  vars[idx] = expr();
}

/* Jump to a line number. Returns the line index, or -1 if it does not exist. */
static int goto_line(num_t n) {
  int i = find_line((uint16_t)n);
  if (i < 0) err("no such line");
  return i;
}

/* Execute one statement. Returns the next line index, or -2 to continue in the
 * current line, or -1 to stop. */
static int statement(void) {
  skipsp();
  if (*cur == 0) return -2;

  if (keyword("REM")) {
    while (*cur) cur++;
    return -2;
  }
  if (keyword("PRINT")) {
    do_print();
    return -2;
  }
  if (keyword("LET")) {
    do_assign();
    return -2;
  }
  if (keyword("INPUT")) {
    do_input();
    return -2;
  }
  if (keyword("CLS")) {
    sys_cls();
    return -2;
  }
  if (keyword("END") || keyword("STOP")) {
    running = 0;
    return -1;
  }
  if (keyword("GOTO")) {
    num_t n = expr();
    if (errmsg) return -1;
    return goto_line(n);
  }
  if (keyword("GOSUB")) {
    num_t n = expr();
    if (errmsg) return -1;
    if (gosub_sp >= GOSUB_DEPTH) {
      err("GOSUB too deep");
      return -1;
    }
    gosub_line[gosub_sp] = (uint16_t)pc;
    gosub_pos[gosub_sp] = (uint16_t)(cur - &pool[ln_off[pc]]);
    gosub_sp++;
    return goto_line(n);
  }
  if (keyword("RETURN")) {
    if (gosub_sp == 0) {
      err("RETURN without GOSUB");
      return -1;
    }
    gosub_sp--;
    pc = (int)gosub_line[gosub_sp];
    cur = &pool[ln_off[pc]] + gosub_pos[gosub_sp];
    return -2;
  }
  if (keyword("IF")) {
    num_t c = expr();
    if (errmsg) return -1;
    skipsp();
    keyword("THEN");
    skipsp();
    if (!c) {
      while (*cur) cur++; /* condition false: skip the rest of the line */
      return -2;
    }
    if (is_digit(*cur)) { /* IF ... THEN <line number> */
      num_t n = 0;
      while (is_digit(*cur)) {
        n = (num_t)(n * 10 + (*cur - '0'));
        cur++;
      }
      return goto_line(n);
    }
    return statement();
  }
  if (keyword("FOR")) {
    int idx;
    num_t start, limit, step = 1;
    skipsp();
    if (!is_alpha(*cur)) {
      err("FOR needs a variable");
      return -1;
    }
    idx = upper(*cur) - 'A';
    cur++;
    skipsp();
    if (*cur != '=') {
      err("expected =");
      return -1;
    }
    cur++;
    start = expr();
    skipsp();
    if (!keyword("TO")) {
      err("expected TO");
      return -1;
    }
    limit = expr();
    skipsp();
    if (keyword("STEP")) step = expr();
    if (errmsg) return -1;
    if (for_sp >= FOR_DEPTH) {
      err("FOR too deep");
      return -1;
    }
    vars[idx] = start;
    fors[for_sp].var = idx;
    fors[for_sp].limit = limit;
    fors[for_sp].step = step;
    fors[for_sp].line = pc;
    fors[for_sp].pos = (uint16_t)(cur - &pool[ln_off[pc]]);
    for_sp++;
    return -2;
  }
  if (keyword("NEXT")) {
    struct forrec *f;
    skipsp();
    if (is_alpha(*cur)) cur++; /* NEXT I — the variable name is decoration */
    if (for_sp == 0) {
      err("NEXT without FOR");
      return -1;
    }
    f = &fors[for_sp - 1];
    vars[f->var] = (num_t)(vars[f->var] + f->step);
    if ((f->step >= 0 && vars[f->var] <= f->limit) ||
        (f->step < 0 && vars[f->var] >= f->limit)) {
      pc = f->line;
      cur = &pool[ln_off[pc]] + f->pos;
      return -2;
    }
    for_sp--;
    return -2;
  }

  do_assign(); /* bare `A = 1` */
  return -2;
}

/* Run the whole program. */
static void cmd_run(void) {
  int next;
  uint16_t ticks = 0;

  gosub_sp = 0;
  for_sp = 0;
  errmsg = 0;
  stop_flag = 0;
  running = 1;
  pc = 0;

  while (running && pc >= 0 && pc < (int)nlines) {
    cur = &pool[ln_off[pc]];
    for (;;) {
      next = statement();
      if (errmsg) {
        running = 0;
        break;
      }
      if (next == -1) {
        running = 0;
        break;
      }
      if (next >= 0) {
        pc = next;
        break;
      }
      skipsp();
      if (*cur == ':') {
        cur++;
        continue;
      }
      if (*cur == 0) {
        pc++;
        break;
      }
      err("syntax");
      running = 0;
      break;
    }

    /* Give the kernel a frame occasionally. The kill switch does not need
     * this — it runs from NMI and cannot be blocked — but it keeps the console
     * and the on-screen keyboard alive during a long computation. */
    if ((++ticks & 0x3F) == 0) sys_yield();
  }

  if (errmsg) {
    outs("? ");
    outs(errmsg);
    if (pc >= 0 && pc < (int)nlines) {
      outs(" at line ");
      outnum((num_t)ln_num[pc]);
    }
    nl();
    errmsg = 0;
  }
}

/* ---- SAVE / LOAD -------------------------------------------------------- */

static void read_name(char *dst) {
  int i = 0;
  skipsp();
  if (*cur == '"') cur++;
  while (*cur && *cur != '"' && i < 15) dst[i++] = upper(*cur++);
  dst[i] = 0;
  if (*cur == '"') cur++;
}

static void cmd_save(void) {
  char name[17];
  int i;
  read_name(name);
  if (name[0] == 0) {
    outs("usage: SAVE \"NAME\"\n");
    return;
  }
  snos_set_ptr(name);
  if (sys_open(1) == SYS_ERR) {
    outs("cannot create\n");
    return;
  }
  /* Every byte checked: the volume can fill mid-save, and reporting 'saved'
   * over a truncated program is how the next LOAD loses half of it. */
  {
    int err = 0;
    for (i = 0; i < (int)nlines && !err; i++) {
      const char *p;
      uint16_t n = ln_num[i];
      char digits[6];
      int d = 0;
      if (n == 0) digits[d++] = '0';
      while (n) {
        digits[d++] = (char)('0' + (n % 10));
        n = (uint16_t)(n / 10);
      }
      while (d && !err)
        err = sys_putc_f((uint16_t)(unsigned char)digits[--d]) == SYS_ERR;
      if (!err) err = sys_putc_f(' ') == SYS_ERR;
      for (p = &pool[ln_off[i]]; *p && !err; p++)
        err = sys_putc_f((uint16_t)(unsigned char)*p) == SYS_ERR;
      if (!err) err = sys_putc_f('\n') == SYS_ERR;
    }
    sys_close();
    if (err) {
      /* Drop the truncated file: a program that LOADs half-way is worse than
       * the honest failure. The previous SAVE is already gone (overwrite),
       * which is exactly why the message must not pretend otherwise. */
      snos_set_ptr(name);
      sys_unlink();
      outs("save failed: volume full, nothing saved\n");
      return;
    }
  }
  outs("saved\n");
}

static void cmd_load(void) {
  char name[17];
  int c, i;
  read_name(name);
  if (name[0] == 0) {
    outs("usage: LOAD \"NAME\"\n");
    return;
  }
  snos_set_ptr(name);
  if (sys_open(0) == SYS_ERR) {
    outs("no such file\n");
    return;
  }
  cmd_new();
  i = 0;
  for (;;) {
    c = (int)sys_getc_f();
    if (c == (int)SYS_ERR || c == '\n') {
      line[i] = 0;
      if (i > 0) {
        const char *p = line;
        uint16_t n = 0;
        while (*p == ' ') p++;
        while (is_digit(*p)) {
          n = (uint16_t)(n * 10 + (*p - '0'));
          p++;
        }
        while (*p == ' ') p++;
        if (n) insert_line(n, p);
      }
      i = 0;
      if (c == (int)SYS_ERR) break;
      continue;
    }
    if (i < LINEBUF - 1) line[i++] = (char)c;
  }
  sys_close();
  outs("loaded\n");
}

static void cmd_files(void) {
  char name[18];
  uint16_t i = 0;
  for (;;) {
    snos_set_ptr(name);
    if (sys_dir(i) == 0) break;
    outs(name);
    nl();
    i++;
  }
}

static void cmd_help(void) {
  outs("statements: PRINT LET IF/THEN GOTO GOSUB RETURN\n");
  outs("            FOR/TO/STEP NEXT INPUT REM CLS END\n");
  outs("commands:   RUN LIST NEW SAVE LOAD FILES HELP\n");
  outs("            BYE / EXIT / QUIT all return to the shell\n");
  outs("vars A-Z, array @(0..63), ops + - * / %% and = <> < > <= >=\n");
  outs("START+SELECT kills a stuck program at any time\n");
}

/* ---- the REPL ----------------------------------------------------------- */

int main(void) {
  rnd_state = 12345;
  cmd_new();
  outs("SNOS BASIC\n");
  outs("integer BASIC - HELP for commands, QUIT to leave\n\n");

  /* ABI 4 launch argument: `basic prog.bas` loads that program before the
   * prompt appears. Without it a saved program could be listed by the shell and
   * never opened from it, which is a strange thing for SAVE to leave you with.
   * A missing file is reported and the interpreter still starts — you asked for
   * BASIC, not for that file specifically. */
  {
    char arg[20];
    if (snos_getargs(arg, (uint16_t)sizeof arg) > 0) {
      cur = arg;
      cmd_load();
    }
  }

  for (;;) {
    outs("] ");
    snos_set_ptr(line);
    sys_readline_raw(LINEBUF);

    cur = line;
    skipsp();
    if (*cur == 0) continue;

    /* A leading line number edits the program instead of executing. */
    if (is_digit(*cur)) {
      uint16_t n = 0;
      while (is_digit(*cur)) {
        n = (uint16_t)(n * 10 + (*cur - '0'));
        cur++;
      }
      skipsp();
      insert_line(n, cur);
      continue;
    }

    if (keyword("BYE") || keyword("EXIT") || keyword("QUIT")) break;
    if (keyword("RUN")) {
      cmd_run();
      continue;
    }
    if (keyword("LIST")) {
      cmd_list();
      continue;
    }
    if (keyword("NEW")) {
      cmd_new();
      continue;
    }
    if (keyword("SAVE")) {
      cmd_save();
      continue;
    }
    if (keyword("LOAD")) {
      cmd_load();
      continue;
    }
    if (keyword("FILES")) {
      cmd_files();
      continue;
    }
    if (keyword("HELP")) {
      cmd_help();
      continue;
    }

    /* Anything else is an immediate statement. */
    pc = -1;
    errmsg = 0;
    running = 0;
    cur = line;
    statement();
    if (errmsg) {
      outs("? ");
      outs(errmsg);
      nl();
      errmsg = 0;
    }
  }

  outs("bye\n");
  return 0;
}
