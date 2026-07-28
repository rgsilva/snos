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

static void cmd_help(void) {
  kcon_puts("help      this list\n");
  kcon_puts("clear     clear the screen\n");
  kcon_puts("mem       memory and cartridge SRAM usage\n");
  kcon_puts("echo ...  print arguments\n");
  kcon_puts("apps      list applications\n");
  kcon_puts("run N [A] run an application, with an argument\n");
  kcon_puts("ls        list files\n");
  kcon_puts("cat NAME  print a file\n");
  kcon_puts("write N T write text T to file N\n");
  kcon_puts("rm NAME   delete a file\n");
  kcon_puts("net       gateway status\n");
  kcon_puts("download URL [NAME]   save a URL to a file\n");
  kcon_puts("debug     diagnostics (debug with no argument lists them)\n");
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

static void cmd_run(char *cmdline) {
  const app_entry_t *a;
  uint16_t rc;
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

static void cmd_net(void) {
  if (!net_up()) {
    kcon_puts("no gateway attached\n");
    kcon_puts("(emulator: run tools/netbridge.lua; hardware: M7)\n");
    return;
  }
  kcon_puts("gateway up. frames ");
  kcon_putu(net_stat_rx_frames);
  kcon_puts(", bad sums ");
  kcon_putu(net_stat_bad_sum);
  kcon_puts(", resends ");
  kcon_putu(net_stat_resent);
  kcon_putc('\n');
}

/* Fetch a URL and print the body.
 *
 * The gateway has already done DNS, TLS, redirects, chunked decoding and gzip,
 * so what arrives here is plain bytes and the only work left is to show them.
 * That division is the entire point of the design: none of those five things
 * would be reasonable on a 3.58 MHz CPU, and TLS would not be possible at all.
 */
static uint8_t get_buf[128];

static void cmd_get(const char *url) {
  int ch, st;
  uint16_t total = 0;
  uint16_t deadline;

  if (*url == 0) {
    kcon_puts("usage: get URL\n");
    return;
  }
  if (!net_up()) {
    kcon_puts("no gateway attached\n");
    return;
  }

  ch = net_fetch(url);
  if (ch < 0) {
    kcon_puts("fetch failed: no free channel\n");
    return;
  }

  /* Wait for the gateway to report a status. Ten seconds is generous for DNS
   * plus a TLS handshake on the far side and still short enough that a wedged
   * gateway does not look like a hung machine. */
  deadline = (uint16_t)(snos_frames + 600u);
  while (net_state(ch) == NET_ST_OPENING) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("timeout waiting for the gateway\n");
      net_close(ch);
      return;
    }
    kbd_idle();
  }

  st = net_state(ch);
  if (st == NET_ST_ERROR) {
    kcon_puts("gateway error ");
    kcon_putu(net_http_status(ch));
    kcon_putc('\n');
    net_close(ch);
    return;
  }

  kcon_puts("HTTP ");
  kcon_putu(net_http_status(ch));
  kcon_putc('\n');

  for (;;) {
    int n = net_read(ch, get_buf, (int)sizeof get_buf);
    int i;
    if (n > 0) {
      for (i = 0; i < n; i++) kcon_putc(get_buf[i]);
      total = (uint16_t)(total + n);
      continue;
    }
    st = net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    /* Any keypress abandons the transfer. A page can be tens of kilobytes and
     * the console prints a line per frame, so without this a mistyped URL means
     * waiting minutes for something you no longer want. */
    if (kbd_getchar_nonblock() >= 0) {
      kcon_puts("\n[aborted]\n");
      break;
    }
    kbd_idle();
  }

  kcon_puts("\n[");
  kcon_putu(total);
  kcon_puts(" bytes]\n");
  net_close(ch);
}

/* Derive a filename from a URL's last path segment.
 *
 * Uppercased because that is the convention on this volume — the ROM packer
 * emits uppercase and BASIC's SAVE writes uppercase. Matching is
 * case-insensitive so this is cosmetic, but a directory listing that mixes the
 * two looks like a bug. */
static void name_from_url(const char *url, char *out) {
  const char *p = url, *seg = url;
  int i = 0;

  /* Skip the scheme so its "//" is not mistaken for a path separator. */
  while (*p) {
    if (p[0] == '/' && p[1] == '/') {
      p += 2;
      seg = p;
      break;
    }
    if (*p == '/') break;
    p++;
  }
  for (; *p; p++) {
    if (*p == '/') seg = p + 1;
    else if (*p == '?' || *p == '#') break; /* query is not part of the name */
  }

  for (; seg < p && *seg && i < FS_NAME_LEN; seg++) {
    char c = *seg;
    if (c == '?' || c == '#') break;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    out[i++] = c;
  }
  out[i] = 0;
  /* "example.com" and "example.com/" both have an empty last segment. */
  if (i == 0) {
    const char *d = "INDEX.HTM";
    while (*d) out[i++] = *d++;
    out[i] = 0;
  }
}

static void cmd_download(char *rest) {
  char nm[FS_NAME_LEN + 1];
  char *given;
  int ch, st;
  uint32_t got = 0, budget, dots = 0;
  uint16_t deadline;
  int aborted = 0, full = 0;

  given = split_word(rest);
  if (*rest == 0) {
    kcon_puts("usage: download URL [NAME]\n");
    return;
  }
  if (!net_up()) {
    kcon_puts("no gateway attached\n");
    return;
  }

  if (*given) {
    int i = 0;
    while (given[i] && i < FS_NAME_LEN) {
      char c = given[i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);
      nm[i] = c;
      i++;
    }
    nm[i] = 0;
  } else {
    name_from_url(rest, nm);
  }

  /* Refuse before starting rather than after filling the volume. */
  budget = fs_free();
  if (budget < 1024ul) {
    kcon_puts("not enough room on the SRAM volume\n");
    return;
  }

  ch = net_fetch(rest);
  if (ch < 0) {
    kcon_puts("fetch failed: no free channel\n");
    return;
  }

  deadline = (uint16_t)(snos_frames + 600u);
  while (net_state(ch) == NET_ST_OPENING) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("timeout waiting for the gateway\n");
      net_close(ch);
      return;
    }
    kbd_idle();
  }

  st = net_state(ch);
  if (st == NET_ST_ERROR) {
    kcon_puts("gateway error ");
    kcon_putu(net_http_status(ch));
    kcon_putc('\n');
    net_close(ch);
    return;
  }
  if (net_http_status(ch) != 200) {
    kcon_puts("HTTP ");
    kcon_putu(net_http_status(ch));
    kcon_puts(" - not saved\n");
    net_close(ch);
    return;
  }

  if (fs_open_write(nm) != 0) {
    kcon_puts("cannot create ");
    kcon_puts(nm);
    kcon_putc('\n');
    net_close(ch);
    return;
  }

  kcon_puts("-> ");
  kcon_puts(nm);
  kcon_putc('\n');

  for (;;) {
    int n, i;

    /* Poll for an abort on EVERY pass, not only when the ring runs dry.
     *
     * The obvious placement — beside the state check further down, where `get`
     * has it — is only reached when no data is waiting. A transfer that streams
     * steadily never takes that branch, so it becomes uninterruptible exactly
     * when it is longest, which is the case this exists for. A shell command is
     * not an application, so START+SELECT cannot rescue it either. */
    if (kbd_getchar_nonblock() >= 0) {
      aborted = 1;
      break;
    }

    n = net_read(ch, get_buf, (int)sizeof get_buf);
    if (n > 0) {
      /* fs_free() cannot fall during a write — the allocator only commits on
       * close — so the remaining room has to be tracked here. */
      if ((uint32_t)n > budget) {
        full = 1;
        break;
      }
      for (i = 0; i < n; i++) {
        if (fs_putc(get_buf[i]) != 0) {
          full = 1;
          break;
        }
      }
      if (full) break;
      got += (uint32_t)n;
      budget -= (uint32_t)n;
      /* One dot per kilobyte, accumulating across the line.
       *
       * Not a byte count repainted in place: that costs a vblank every time it
       * is redrawn, and the link is skipped on any frame the console is still
       * dirty, so a livelier readout directly slows the transfer it is
       * reporting. A dot is one character, and a wrapping wall of them still
       * says "moving, and roughly how far". */
      while (dots < (got >> 10)) {
        kcon_putc('.');
        dots++;
        con_flush();
      }
      continue;
    }
    st = net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    kbd_idle();
  }

  kcon_putc('\n');
  fs_close();

  if (full) kcon_puts("[volume full - partial file kept]\n");
  else if (aborted) kcon_puts("[aborted - partial file kept]\n");
  else {
    kcon_puts("saved ");
    kcon_puts(nm);
    kcon_puts(", ");
    kcon_putu32(got);
    kcon_puts(" bytes\n");
  }
  net_close(ch);
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
  else if (streq(what, "get")) cmd_get(arg);
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
    else if (streq(line, "net")) cmd_net();
    else if (streq(line, "debug")) cmd_debug(rest);
    else if (streq(line, "download")) cmd_download(rest);
    else {
      kcon_puts("unknown command: ");
      kcon_puts(line);
      kcon_puts("\ntype 'help'\n");
    }
  }
}
