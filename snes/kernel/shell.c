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

static char line[128];

/* Hand-rolled rather than <string.h>: the kernel stack is 2816 bytes and the
 * point of never linking the C library here is to keep it that way. */
static int streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) return 0;
    a++;
    b++;
  }
  return *a == *b;
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
  kcon_puts("ver       version and ABI\n");
  kcon_puts("clear     clear the screen\n");
  kcon_puts("mem       memory map and stack watermark\n");
  kcon_puts("echo ...  print arguments\n");
  kcon_puts("syscheck  exercise the COP syscall path\n");
  kcon_puts("apps      list applications\n");
  kcon_puts("run NAME  run an application\n");
  kcon_puts("ls        list files\n");
  kcon_puts("cat NAME  print a file\n");
  kcon_puts("write N T write text T to file N\n");
  kcon_puts("rm NAME   delete a file\n");
  kcon_puts("sram      cartridge SRAM usage\n");
  kcon_puts("fstest    verify SRAM addressing\n");
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

static void cmd_run(const char *name) {
  const app_entry_t *a;
  uint16_t rc;

  if (*name == 0) {
    kcon_puts("usage: run NAME\n");
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
    kcon_puts(f->ro ? "  rom\n" : "  sram\n");
  }
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
  for (i = 0; i < f->size; i++) kcon_putc(fs_read_byte(f, i));
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

static void cmd_sram(void) {
  kcon_puts("cart SRAM ");
  kcon_putu((uint16_t)(sram_size() >> 10));
  kcon_puts(" KB, free ");
  kcon_putu((uint16_t)(fs_free() >> 10));
  kcon_puts(" KB\n");
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

/* ---------------------------------------------------------------- shell */

void shell_run(void) {
  kcon_puts("type 'help' for commands\n\n");
  for (;;) {
    char *rest;
    kcon_puts("snos> ");
    kbd_readline(line, (int)sizeof line);
    rest = split_word(line);

    if (line[0] == 0) continue;
    if (streq(line, "help")) cmd_help();
    else if (streq(line, "ver")) cmd_ver();
    else if (streq(line, "clear")) con_clear();
    else if (streq(line, "mem")) cmd_mem();
    else if (streq(line, "echo")) cmd_echo(rest);
    else if (streq(line, "syscheck")) cmd_syscheck();
    else if (streq(line, "apps")) cmd_apps();
    else if (streq(line, "run")) cmd_run(rest);
    else if (streq(line, "ls")) cmd_ls();
    else if (streq(line, "cat")) cmd_cat(rest);
    else if (streq(line, "write")) cmd_write(rest);
    else if (streq(line, "rm")) cmd_rm(rest);
    else if (streq(line, "sram")) cmd_sram();
    else if (streq(line, "fstest")) cmd_fstest();
    else {
      kcon_puts("unknown command: ");
      kcon_puts(line);
      kcon_puts("\ntype 'help'\n");
    }
  }
}
