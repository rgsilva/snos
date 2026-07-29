/* SNOS shell.
 *
 * Deliberately dumb: read a line, match the first word, run it. There is no
 * scripting, no pipes, no globbing. The shell's job is to launch things
 * (M3 onward) and to make the machine's state legible.
 *
 * The shell is kernel code and calls the kernel directly, EXCEPT in `syscheck`,
 * which goes through the real `COP` thunks. That is on purpose: it keeps the
 * everyday path fast while still exercising the syscall ABI end to end from a
 * caller, so the trap frame decoding is tested before any app depends on it.
 */
#include "snos.h"
#include "snos_abi.h"
#include "console.h"
#include "input.h"
#include "mailbox.h"
#include "app.h"
#include "fs.h"
#include "sram.h"
#include "net.h"

static char line[128];

/* Hand-rolled rather than <string.h>: the kernel stack is 2816 bytes and the
 * point of never linking the C library here is to keep it that way.
 *
 * Case-insensitive, matching filenames and application names (fs.c, app.c).
 * Typing an uppercase letter on this machine means holding shift on an
 * on-screen keyboard, so insisting on a particular case is a real cost paid for
 * nothing. The comparison is against a literal that is always lowercase, so
 * only the user's side needs folding — one compare and one conditional subtract
 * per character, on a string that is at most a handful of bytes and is compared
 * once per typed line. It is not measurable. */
static int streq(const char *typed, const char *lower) {
  while (*typed && *lower) {
    char c = *typed;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != *lower) return 0;
    typed++;
    lower++;
  }
  return *typed == *lower;
}

/* Split off the first word in place; returns the rest (may be empty). */
static char *split_word(char *s) {
  while (*s == ' ') s++;
  {
    char *w = s;
    while (*s && *s != ' ') s++;
    if (*s) {
      *s++ = 0;
      while (*s == ' ') s++;
    }
    (void)w;
  }
  return s;
}

static void put_kv(const char *k, uint16_t v) {
  kcon_puts(k);
  kcon_puts(" $");
  kcon_puthex16(v);
  kcon_putc('\n');
}

/* ------------------------------------------------------------- builtins */

/* One column for every description, wide enough for the longest usage on the
 * left. It had grown two: the original entries at column 10 and later ones at
 * column 22, so the descriptions no longer lined up and the list read as two
 * lists. 14 fits "write N T" and "run N [A]" with room, inside 64 columns. */
static void cmd_help(void) {
  kcon_puts("help          this list\n");
  kcon_puts("clear         clear the screen\n");
  kcon_puts("mem           memory and cartridge SRAM usage\n");
  kcon_puts("echo ...      print arguments\n");
  kcon_puts("apps          list applications\n");
  kcon_puts("run N [A]     run an application, with an argument\n");
  kcon_puts("ls            list files\n");
  kcon_puts("cat NAME      print a file\n");
  kcon_puts("write N T     write text T to file N\n");
  kcon_puts("rm NAME       delete a file\n");
  kcon_puts("net           gateway diagnostics\n");
  kcon_puts("debug         other diagnostics\n");
}

static void cmd_ver(void) {
  kcon_puts("SNOS " SNOS_VERSION_STR ", kernel ABI ");
  kcon_putu(SNOS_ABI_VERSION);
  kcon_puts("\n65816 native, HiROM fastrom, no SA-1\n");
}

/* The bank-$00 budget is the design's binding constraint, so the shell reports
 * it directly rather than making anyone go and read linker.scm. */
static void cmd_mem(void) {
  uint16_t low = stack_low();
  kcon_puts("bank $00 (8K, holds every stack and direct page):\n");
  put_kv("  kernel DP    ", SNOS_KDP_BASE);
  put_kv("  kernel stack ", SNOS_KSTACK_LO);
  put_kv("  app stack    ", SNOS_ASTACK_LO);
  put_kv("  app DP       ", SNOS_ADP_BASE);
  put_kv("  syscall args ", (uint16_t)(SNOS_ARGS_ADDR & 0xFFFFu));
  kcon_puts("kernel stack used ");
  kcon_putu((uint16_t)(SNOS_KSTACK_HI + 1u - low));
  kcon_puts(" of ");
  kcon_putu((uint16_t)(SNOS_KSTACK_HI + 1u - SNOS_KSTACK_LO));
  kcon_puts(" bytes\n");
  kcon_puts("app arena 64K at $7F0000\n");
  /* Cartridge SRAM used to be a separate `sram` command. It is the same
   * question — where does memory go — so it is the same command. */
  kcon_puts("cart SRAM ");
  kcon_putu((uint16_t)(sram_size() >> 10));
  kcon_puts(" KB, free ");
  kcon_putu((uint16_t)(fs_free() >> 10));
  kcon_puts(" KB\n");
}

static void cmd_echo(const char *rest) {
  kcon_puts(rest);
  kcon_putc('\n');
}

/* Everything here goes through COP. If the trap frame decoding, the signature
 * read-back, or the return-value path is wrong, this is what says so. */
static const char SYSCHECK_PUTS[] = "puts-ok";
static const char SYSCHECK_TRACE[] = "SYSCHECK-TRACE\n";

static void set_ptr_arg(const char *s) {
  uint32_t p = (uint32_t)s;
  SNOS_ARGS.p0 = (uint16_t)p;
  SNOS_ARGS.p1 = (uint16_t)(p >> 16);
}

static void cmd_syscheck(void) {
  uint16_t v;

  v = sys_version();
  kcon_puts("version=");
  kcon_putu(v);
  kcon_puts(v == SNOS_ABI_VERSION ? " ok\n" : " BAD\n");

  kcon_puts("putc=");
  sys_putc('Z');
  kcon_putc('\n');

  kcon_puts("puts=");
  set_ptr_arg(SYSCHECK_PUTS);
  sys_puts_raw();
  kcon_putc('\n');

  kcon_puts("putu=");
  sys_putu(12345);
  kcon_putc('\n');

  kcon_puts("puthex=");
  sys_puthex(0xBEEF);
  kcon_putc('\n');

  v = sys_frames();
  kcon_puts("frames=");
  kcon_putu(v);
  kcon_putc('\n');

  /* An unimplemented call must report failure rather than doing something
   * arbitrary — apps built against a newer ABI have to be able to tell. */
  set_ptr_arg(SYSCHECK_TRACE);
  sys_trace_raw();

  kcon_puts("syscheck done\n");
}

static void cmd_apps(void) {
  int i, n = app_count();
  if (n == 0) {
    kcon_puts("no applications in this ROM\n");
    return;
  }
  for (i = 0; i < n; i++) {
    const app_entry_t *a = app_get(i);
    kcon_puts(a->name);
    kcon_puts("  bank $");
    kcon_puthex16(a->bank);
    kcon_puts(" entry $");
    kcon_puthex16(a->entry);
    kcon_putc('\n');
  }
}

/* Hand the rest of the command line to the application it launches.
 *
 * Always written, even when empty: an app given no argument must see an empty
 * string rather than whatever the last one was launched with. */
static void set_argv(const char *s) {
  volatile __far char *dst = (volatile __far char *)SNOS_ARGV_ADDR;
  uint16_t i = 0;
  while (s && s[i] && i + 1 < SNOS_ARGV_MAX) {
    dst[i] = s[i];
    i++;
  }
  dst[i] = 0;
}

/* Shared by `run NAME` and by typing an application's name directly. Factored
 * so the two paths cannot drift — an ABI check that guards one entry point and
 * not the other is worse than no check at all. */
static void run_app_entry(const app_entry_t *a, const char *args) {
  uint16_t rc;

  if (a->minabi > SNOS_ABI_VERSION) {
    /* An app built against a newer kernel must be refused rather than run and
     * allowed to fall over on a syscall that does not exist yet. */
    kcon_puts("needs kernel ABI ");
    kcon_putu(a->minabi);
    kcon_puts(", have ");
    kcon_putu(SNOS_ABI_VERSION);
    kcon_putc('\n');
    return;
  }

  set_argv(args);
  rc = app_run(a);

  kcon_puts("[");
  kcon_puts(a->name);
  if (rc == SNOS_EXIT_KILLED) kcon_puts(" killed]\n");
  else {
    kcon_puts(" exited ");
    kcon_putu(rc);
    kcon_puts("]\n");
  }
}

static void cmd_run(char *cmdline) {
  const app_entry_t *a;
  char *name = cmdline;
  const char *args = split_word(cmdline);

  if (*name == 0) {
    kcon_puts("usage: run NAME [ARGS]\n");
    return;
  }
  a = app_find(name);
  if (a == 0) {
    kcon_puts("no such application: ");
    kcon_puts(name);
    kcon_putc('\n');
    return;
  }
  run_app_entry(a, args);
}

/* Not a builtin? Try an application of that name.
 *
 * This is what makes `get` viable as an application rather than a shell
 * command: `get URL DEST` has to work without a `run` prefix, or moving it out
 * of the kernel would have made it worse to use. It generalises for free —
 * `edit FILE`, `view PIC.IMG` and `basic` all work now — and `run` stays for
 * when you want to be explicit or when an app shares a name with a builtin.
 *
 * Returns 0 if there is no such application, so the caller still owns the
 * unknown-command message. */
static void cmd_ls(void) {
  int i, n = fs_count();
  if (n == 0) {
    kcon_puts("no files\n");
    return;
  }
  for (i = 0; i < n; i++) {
    const fs_entry_t *f = fs_get(i);
    int k = 0;
    kcon_puts(f->name);
    while (f->name[k]) k++;
    while (k++ < 18) kcon_putc(' ');
    kcon_putu((uint16_t)f->size);
    if (!f->ro) kcon_puts("  sram\n");
    else if (fs_shadowed(i)) kcon_puts("  rom (hidden)\n");
    else kcon_puts("  rom\n");
  }
}

/* Wait between pages. Returns 0 if the reader wants out.
 *
 * The prompt is drawn and then erased in place with a carriage return, so it
 * costs no line of its own — otherwise every page would lose a row of the file
 * to the furniture. */
static int pager_more(void) {
  int c;
  kcon_puts("-- more -- (q quits)");
  kcon_sync();
  c = kbd_getchar();
  kcon_putc('\r');
  kcon_puts("                    ");
  kcon_putc('\r');
  return (c == 'q' || c == 'Q' || c == 0x1B) ? 0 : 1;
}

static void cmd_cat(const char *name) {
  const fs_entry_t *f;
  uint32_t i;
  if (*name == 0) {
    kcon_puts("usage: cat NAME\n");
    return;
  }
  f = fs_find(name);
  if (f == 0) {
    kcon_puts("no such file: ");
    kcon_puts(name);
    kcon_putc('\n');
    return;
  }
  {
    /* Page it. README.TXT is 592 bytes and scrolls most of itself away
     * unpaged, which makes the one document explaining the system the one you
     * cannot read.
     *
     * Rows consumed are counted here rather than read back from the console,
     * because once the display starts scrolling the cursor row saturates at the
     * bottom and stops being a measure of anything. This mirrors con_putc's own
     * line accounting: newline, carriage return, backspace, and the wrap at the
     * right margin. */
    int rows = con_text_rows();
    int cols = con_cols();
    int used = 0, col = 0;

    for (i = 0; i < f->size; i++) {
      uint8_t c = fs_read_byte(f, i);
      kcon_putc(c);

      if (c == '\n') {
        col = 0;
        used++;
      } else if (c == '\r') {
        col = 0;
      } else if (c == '\b') {
        if (col) col--;
      } else if (c >= 0x20) {
        if (++col >= cols) {
          col = 0;
          used++;
        }
      }

      /* Only prompt when there is genuinely more to show: a file that ends
       * exactly on a screen boundary should not ask. */
      if (used >= rows - 1 && i + 1 < f->size) {
        if (!pager_more()) return;
        used = 0;
        col = 0;
      }
    }
  }
  kcon_putc('\n');
}

/* write NAME text... — the simplest possible way to put a file on the SRAM
 * volume without an editor. */
static void cmd_write(char *rest) {
  char *text = split_word(rest);
  if (*rest == 0) {
    kcon_puts("usage: write NAME text\n");
    return;
  }
  if (fs_open_write(rest) != 0) {
    kcon_puts("cannot create ");
    kcon_puts(rest);
    kcon_putc('\n');
    return;
  }
  while (*text) fs_putc((uint8_t)*text++);
  fs_putc('\n');
  fs_close();
  kcon_puts("wrote ");
  kcon_puts(rest);
  kcon_putc('\n');
}

static void cmd_rm(const char *name) {
  if (*name == 0) {
    kcon_puts("usage: rm NAME\n");
    return;
  }
  if (fs_delete(name) != 0) {
    kcon_puts("cannot delete ");
    kcon_puts(name);
    kcon_puts(" (missing, or it is in ROM)\n");
    return;
  }
  kcon_puts("deleted ");
  kcon_puts(name);
  kcon_putc('\n');
}

/* Write a known pattern to the SRAM volume and read it back byte for byte.
 *
 * This exists because SRAM is not flat memory: it is 8 KB windows in separate
 * banks, and the linear-offset arithmetic has to cross those boundaries
 * correctly. 20000 bytes spans three windows, so a mistake in the window or
 * bank calculation shows up here instead of quietly eating someone's file. */
#define FSTEST_LEN 20000ul

static uint8_t fstest_byte(uint32_t i) {
  return (uint8_t)((i * 7u) ^ (uint8_t)(i >> 8));
}

static void cmd_fstest(void) {
  const fs_entry_t *f;
  uint32_t i, bad = 0, first_bad = 0;

  if (fs_open_write("FSTEST.BIN") != 0) {
    kcon_puts("fstest: cannot create\n");
    return;
  }
  for (i = 0; i < FSTEST_LEN; i++) fs_putc(fstest_byte(i));
  fs_close();

  f = fs_find("FSTEST.BIN");
  if (f == 0) {
    kcon_puts("fstest: file vanished after write\n");
    return;
  }
  if (f->size != FSTEST_LEN) {
    kcon_puts("fstest: size ");
    kcon_putu((uint16_t)f->size);
    kcon_puts(" hi ");
    kcon_puthex16((uint16_t)(f->size >> 16));
    kcon_puts(" wrote ");
    kcon_putu((uint16_t)i);
    kcon_putc('\n');
    return;
  }
  for (i = 0; i < FSTEST_LEN; i++) {
    if (fs_read_byte(f, i) != fstest_byte(i)) {
      if (bad == 0) first_bad = i;
      bad++;
    }
  }
  fs_delete("FSTEST.BIN");

  if (bad == 0) {
    kcon_puts("fstest: 20000 bytes across 3 windows OK\n");
  } else {
    kcon_puts("fstest: ");
    kcon_putu((uint16_t)bad);
    kcon_puts(" bad bytes, first at ");
    kcon_putu((uint16_t)first_bad);
    kcon_putc('\n');
  }
}

/* Ask the far end to prove it is there, rather than reporting the mailbox.
 *
 * net_up() only tests whether the *bridge* claimed the link. The daemon behind
 * it can be stopped, crashed or wedged and this still said "gateway up" — which
 * it cheerfully did during a session where every fetch was timing out. A status
 * line that is right only when nothing is wrong is worse than none, because it
 * is consulted precisely when something is. */
static void cmd_net_status(void) {
  uint16_t before, deadline, waited;

  if (!net_up()) {
    kcon_puts("no link (the bridge script is not running)\n");
    kcon_puts("(emulator: run build/snosbridge.lua; hardware: M7)\n");
    return;
  }

  before = net_stat_hello;
  if (net_hello() < 0) {
    kcon_puts("link up, but the send queue is full\n");
    return;
  }

  /* One second. A healthy round trip is a couple of frames — the daemon answers
   * HELLO from the frame thread, so even a fetch in progress cannot delay it. */
  deadline = (uint16_t)(snos_frames + 60u);
  while (net_stat_hello == before) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("link up, but the gateway is NOT answering\n");
      kcon_puts("(the bridge is running; start tools/gateway.py)\n");
      return;
    }
    kbd_idle();
  }
  waited = (uint16_t)(snos_frames - (uint16_t)(deadline - 60u));

  kcon_puts("gateway alive, v");
  kcon_putu(net_gw_version);
  kcon_puts(", replied in ");
  kcon_putu(waited);
  kcon_puts(" frames\n");
  kcon_puts("frames ");
  kcon_putu(net_stat_rx_frames);
  kcon_puts(", bad sums ");
  kcon_putu(net_stat_bad_sum);
  kcon_puts(", resends ");
  kcon_putu(net_stat_resent);
  kcon_putc('\n');
}

/* `net test` — prove the whole path works, end to end.
 *
 * Deliberately accepts ANY response, including an error page. The target is
 * behind Cloudflare and will refuse most non-browser requests, which is fine:
 * getting *anything* back means DNS resolved, TCP connected, TLS completed and
 * the link carried bytes in both directions. Asserting on the content would
 * test Cloudflare's mood rather than our networking. */
static const char NET_TEST_URL[] = "https://rgsilva.com/";

static uint8_t net_test_buf[64];

static void cmd_net_test(void) {
  int ch, st;
  uint32_t got = 0;
  uint16_t deadline, http;

  if (!net_up()) {
    kcon_puts("fail (no gateway)\n");
    return;
  }
  kcon_puts("testing...\n");

  ch = net_fetch(NET_TEST_URL);
  if (ch < 0) {
    kcon_puts("fail (no free channel)\n");
    return;
  }

  deadline = (uint16_t)(snos_frames + 900u);
  while (net_state(ch) == NET_ST_OPENING) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("fail (timed out)\n");
      net_close(ch);
      return;
    }
    kbd_idle();
  }

  for (;;) {
    int n = net_read(ch, net_test_buf, (int)sizeof net_test_buf);
    if (n > 0) {
      got += (uint32_t)n;
      continue;
    }
    st = net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    if ((int16_t)(snos_frames - deadline) >= 0) break;
    kbd_idle();
  }
  http = net_http_status(ch);
  net_close(ch);

  /* Any reply is a pass, including an error page. The target sits behind
   * Cloudflare and will refuse most non-browser requests; that refusal still
   * proves DNS resolved, TCP connected, TLS completed and the link carried
   * bytes both ways. Asserting on the content would test Cloudflare's mood. */
  if (got > 0) {
    kcon_puts("pass (http ");
    kcon_putu(http);
    kcon_puts(", ");
    kcon_putu32(got);
    kcon_puts(" bytes)\n");
  } else {
    kcon_puts("fail (http ");
    kcon_putu(http);
    kcon_puts(", 0 bytes)\n");
  }
}

static void cmd_net(char *rest) {
  char *what = rest;
  (void)split_word(rest);
  if (streq(what, "status")) cmd_net_status();
  else if (streq(what, "test")) cmd_net_test();
  else {
    kcon_puts("net status    gateway link and counters\n");
    kcon_puts("net test      fetch a known URL to prove the path works\n");
  }
}

/* ---------------------------------------------------------------- shell */

/* Diagnostics live behind `debug` rather than cluttering the root namespace.
 *
 * `get` is here because `download` replaced it for anything real: printing a
 * URL to the console is now a way to check the network works, not a way to
 * fetch something you want. */
static void cmd_debug(char *rest) {
  char *what = rest;
  const char *arg = split_word(rest);

  if (*what == 0) {
    kcon_puts("debug ver       version and ABI\n");
    kcon_puts("debug syscheck  exercise the COP syscall path\n");
    kcon_puts("debug fstest    verify SRAM addressing\n");
    kcon_puts("debug get URL   fetch a URL to the console\n");
    return;
  }
  if (streq(what, "ver")) cmd_ver();
  else if (streq(what, "syscheck")) cmd_syscheck();
  else if (streq(what, "fstest")) cmd_fstest();
  else {
    kcon_puts("unknown debug command: ");
    kcon_puts(what);
    kcon_puts("\ntype 'debug'\n");
  }
}

void shell_run(void) {
  /* Started here rather than in main() only because main.c is being edited
   * concurrently; it belongs beside the other subsystem inits. Safe regardless:
   * net_service() is guarded by a readiness flag, so the NMI handler does
   * nothing until this runs. */
  net_init();

  kcon_puts("type 'help' for commands\n\n");
  for (;;) {
    char *rest;
    kcon_puts("> ");
    kbd_readline(line, (int)sizeof line);
    rest = split_word(line);

    if (line[0] == 0) continue;
    if (streq(line, "help")) cmd_help();
    else if (streq(line, "clear")) con_clear();
    else if (streq(line, "mem")) cmd_mem();
    else if (streq(line, "echo")) cmd_echo(rest);
    else if (streq(line, "apps")) cmd_apps();
    else if (streq(line, "run")) cmd_run(rest);
    else if (streq(line, "ls")) cmd_ls();
    else if (streq(line, "cat")) cmd_cat(rest);
    else if (streq(line, "write")) cmd_write(rest);
    else if (streq(line, "rm")) cmd_rm(rest);
    else if (streq(line, "net")) cmd_net(rest);
    else if (streq(line, "debug")) cmd_debug(rest);
    else {
      /* Not a builtin? Try an application of that name — this is what lets
       * `get URL DEST`, `edit FILE` and `view PIC.IMG` work without a `run`
       * prefix, which is what made moving `get` out of the kernel worthwhile.
       *
       * Written inline rather than as a helper on purpose. Factored into
       * try_run_app(), the launched application crashed before reaching main,
       * deterministically, with a jump target that varied run to run — the app
       * pointer itself arriving corrupt. The identical find-then-launch
       * sequence inside cmd_run() has always worked. Both are far pointers
       * through Calypsi's shared direct-page scratch, and the extra call frame
       * is the only difference; keeping the lookup and the launch in one
       * function is the shape that is known to survive. */
      const app_entry_t *app = app_find(line);
      if (app != 0) {
        run_app_entry(app, rest);
      } else {
        kcon_puts("unknown command: ");
        kcon_puts(line);
        kcon_puts("\ntype 'help'\n");
      }
    }
  }
}
