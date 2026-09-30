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
#include "ip.h"
#include "proc.h"

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
  kcon_puts("restart       reboot SNOS, preserving files\n");
  kcon_puts("mem           memory and cartridge SRAM usage\n");
  kcon_puts("echo ...      print arguments\n");
  kcon_puts("apps          list applications\n");
  kcon_puts("run N [A] [&] run an application; & = in the background\n");
  kcon_puts("jobs          list background jobs\n");
  kcon_puts("kill N        kill background job N\n");
  kcon_puts("fg N          bring job N to the console (exits back here)\n");
  kcon_puts("ls            list files\n");
  kcon_puts("cat NAME      print a file\n");
  kcon_puts("write N T     write text T to file N\n");
  kcon_puts("rm NAME       delete a file (instant; bytes stay spent)\n");
  kcon_puts("compact       recopy files, reclaim deleted space (slow)\n");
  kcon_puts("net           gateway diagnostics\n");
  kcon_puts("ping HOST     ICMP echo, composed on this machine (M11)\n");
  kcon_puts("resolve NAME  DNS, asked and parsed by this machine (M11)\n");
  kcon_puts("debug         other diagnostics\n");
}

/* Only the shell may abandon the kernel stack. A foreground app first comes
 * back through the existing kill-switch/break unwind; background jobs are
 * pinned and killed before the runtime can reset anything beneath them. */
static void cmd_restart(void) {
  uint8_t id;
  if (proc_current() != 0 || SNOS_EXEC_RUN) return;
  proc_lock();
  for (id = 1; id < PROC_MAX; id++) proc_kill(id);
  snos_restart();
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
  put_kv("  proc0 stack  ", SNOS_PSTACK_LO(0));
  put_kv("  proc0 DP     ", SNOS_PDP(0));
  put_kv("  proc1 stack  ", SNOS_PSTACK_LO(1));
  put_kv("  proc1 DP     ", SNOS_PDP(1));
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
  kcon_puts(" KB, reclaimable ");
  kcon_putu((uint16_t)(fs_reclaimable() >> 10));
  kcon_puts(" KB (see compact)\n");
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

  /* M10: a foreground app whose arena overlaps a live background process is
   * refused HERE, deterministically, with a name — never a corruption at
   * runtime. This check plus the build-time slot enforcement is the whole
   * co-run safety story. */
  {
    int c = proc_arena_conflict(a->arena_page, a->arena_pages);
    if (c >= 0) {
      kcon_puts("arena conflict: [");
      kcon_putu((uint16_t)c);
      kcon_puts("] ");
      kcon_puts(proc_name((uint8_t)c));
      kcon_puts(" holds that memory; kill it first\n");
      return;
    }
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

  /* Reclaim what it left behind. Apps are supposed to close their own streams
   * and mostly do, but the break key kills one mid-transfer by design, and
   * channels are reused — so a leak hands the next application someone else's
   * bytes. Say so rather than cleaning up silently: a count that is normally
   * zero is worth noticing on the day it is not. */
  {
    int leaked = net_reap(1); /* tag 1 = the foreground app */
    if (leaked > 0) {
      kcon_puts("[");
      kcon_puts(a->name);
      kcon_puts(" left ");
      kcon_putu((uint16_t)leaked);
      kcon_puts(leaked == 1 ? " connection open - closed]\n"
                            : " connections open - closed]\n");
    }
  }
}

/* `run NAME ... &`: the app becomes a BACKGROUND process (M10). It runs
 * muted — no console, no input, no filesystem handle — which suits the apps
 * worth backgrounding: servers and connections that poll. */
static void run_app_bg(const app_entry_t *a, const char *args) {
  int id;
  if (a->minabi > SNOS_ABI_VERSION) {
    kcon_puts("needs a newer kernel ABI\n");
    return;
  }
  {
    int c = proc_arena_conflict(a->arena_page, a->arena_pages);
    if (c >= 0) {
      kcon_puts("arena conflict: [");
      kcon_putu((uint16_t)c);
      kcon_puts("] ");
      kcon_puts(proc_name((uint8_t)c));
      kcon_puts(" holds that memory\n");
      return;
    }
  }
  set_argv(args);
  id = proc_spawn_app(a);
  if (id < 0) {
    kcon_puts("no free process slot (one background job for now)\n");
    return;
  }
  /* argv lives in ONE shared block and the new process reads it after its
   * crt0 zeroes the arena. Waiting out that window here is what keeps a
   * fast next command from overwriting the arguments mid-handoff. */
  {
    uint16_t until = (uint16_t)(snos_frames + 60u);
    while ((int16_t)(snos_frames - until) < 0) {
    }
  }
  kcon_puts("[");
  kcon_putu((uint16_t)id);
  kcon_puts("] ");
  kcon_puts(a->name);
  kcon_puts(" started in the background\n");
}

/* Trailing " &" on the command line = background. Stripped in place. */
static int strip_amp(char *args) {
  int i = 0;
  while (args[i]) i++;
  while (i > 0 && args[i - 1] == ' ') i--;
  if (i > 0 && args[i - 1] == '&') {
    i--;
    while (i > 0 && args[i - 1] == ' ') i--;
    args[i] = 0;
    return 1;
  }
  return 0;
}

static void cmd_run(char *cmdline) {
  const app_entry_t *a;
  char *name = cmdline;
  char *args = split_word(cmdline);
  int bg = strip_amp(args) | strip_amp(name); /* `run pad &` with no args */

  if (*name == 0) {
    kcon_puts("usage: run NAME [ARGS] [&]\n");
    return;
  }
  a = app_find(name);
  if (a == 0) {
    kcon_puts("no such application: ");
    kcon_puts(name);
    kcon_putc('\n');
    return;
  }
  if (bg) run_app_bg(a, args);
  else run_app_entry(a, args);
}

static void cmd_jobs(void) {
  uint8_t i;
  int any = 0;
  for (i = 1; i < PROC_MAX; i++) {
    uint8_t st = proc_state(i);
    if (st == 0) continue;
    any = 1;
    kcon_puts("[");
    kcon_putu(i);
    kcon_puts("] ");
    kcon_puts(proc_name(i));
    if (st == 1) {
      kcon_puts("  running\n");
    } else {
      kcon_puts(proc_exit_code(i) == SNOS_EXIT_KILLED ? "  killed\n"
                                                      : "  exited\n");
      proc_reap_slot(i); /* reported once, then the slot is free again */
    }
  }
  if (!any) kcon_puts("no background jobs\n");
}

/* Bring a background process to the console. The shell hands over the
 * foreground (its own console/input syscalls stay direct, but it prints
 * nothing while it waits), then blocks until the process dies -- exit or
 * ^Q inside the app, or the keyboard break. The wait loop is itself a
 * switch point (kbd_idle -> proc_switch_now), which is what makes this not
 * a deadlock. A freshly foregrounded full-screen app repaints on its next
 * event; irc's ^N is the conventional nudge. */
static void cmd_fg(char *rest) {
  uint16_t id = 0;
  char *w = rest;
  (void)split_word(rest);
  while (*w >= '0' && *w <= '9') id = (uint16_t)(id * 10 + (*w++ - '0'));
  /* id 0 is the shell itself: waiting for our own death is not a feature. */
  if (id < 1 || proc_state((uint8_t)id) != 1) {
    kcon_puts("no such job (see `jobs`)\n");
    return;
  }
  proc_set_fg((uint8_t)id);
  while (proc_state((uint8_t)id) == 1) {
    /* The keyboard break must still work with the shell asleep: F12 sets
     * snos_break (no fg app in the exec sense), and it means "kill the
     * foregrounded job, give me my shell back". */
    if (snos_break) {
      snos_break = 0;
      proc_kill((uint8_t)id);
      net_reap((uint8_t)(id + 1));
      break;
    }
    kbd_idle();
  }
  proc_set_fg(0);
  kcon_puts("[");
  kcon_putu(id);
  kcon_puts("] ");
  kcon_puts(proc_name((uint8_t)id));
  kcon_puts(proc_exit_code((uint8_t)id) == SNOS_EXIT_KILLED ? " killed\n"
                                                            : " exited\n");
  proc_reap_slot((uint8_t)id);
}

static void cmd_kill(char *rest) {
  uint16_t id = 0;
  char *w = rest;
  (void)split_word(rest);
  while (*w >= '0' && *w <= '9') id = (uint16_t)(id * 10 + (*w++ - '0'));
  if (proc_kill((uint8_t)id) < 0) {
    kcon_puts("no such job (see `jobs`)\n");
    return;
  }
  net_reap((uint8_t)(id + 1)); /* its connections die with it */
  kcon_puts("[");
  kcon_putu(id);
  kcon_puts("] killed\n");
  proc_reap_slot((uint8_t)id);
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
  /* Erasing is carriage returns and spaces — no newline — so nothing about it
   * makes the console decide to paint. It used to ride out on the next line of
   * the file, which is invisible until the page you quit on: then there is no
   * next line and the prompt stays on screen. Say so explicitly rather than
   * relying on somebody else's output to carry it. */
  kcon_sync();
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
  /* Checked per byte, like every writer since the review: the volume can
   * fill mid-write, and 'wrote NAME' over a truncated file is the lie that
   * cost edit and BASIC their fix in the same wave. */
  {
    int werr = 0;
    while (*text && !werr) werr = fs_putc((uint8_t)*text++) != 0;
    if (!werr) werr = fs_putc('\n') != 0;
    fs_close();
    if (werr) {
      fs_delete(rest); /* drop the truncated file */
      kcon_puts("write failed: volume full, nothing written\n");
      return;
    }
  }
  kcon_puts("wrote ");
  kcon_puts(rest);
  kcon_putc('\n');
}

/* Recopy the surviving files downward and rewind the allocator. Slow and
 * deliberate -- see fs_compact for the journal that makes it crash-safe. */
static void cmd_compact(void) {
  uint32_t before = fs_reclaimable();
  int moved;
  kcon_puts("compacting - do not power off...\n");
  kcon_sync();
  moved = fs_compact();
  if (moved < 0) {
    kcon_puts("cannot compact while a file is open for writing\n");
    return;
  }
  kcon_puts("reclaimed ");
  kcon_putu((uint16_t)(before >> 10));
  kcon_puts(" KB (");
  kcon_putu((uint16_t)moved);
  kcon_puts(" files moved)\n");
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
  /* The pages stay spent (bump allocator); say so when it starts to matter,
   * and leave the decision with the user -- rm stays instant and safe. */
  if (fs_reclaimable() >= 8u * 1024u) {
    kcon_putu((uint16_t)(fs_reclaimable() >> 10));
    kcon_puts(" KB reclaimable - run 'compact' to recover it\n");
  }
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

  /* Overwriting on a full volume must refuse BEFORE deleting: with a bump
   * allocator "full" is the steady state, so overwrite-then-fail would make
   * "both versions lost" the common failure. Force the allocator full and
   * demand both the refusal and the survivor. */
  {
    uint32_t saved;
    if (fs_open_write("OVR.TXT") != 0) {
      kcon_puts("fstest: cannot create OVR.TXT\n");
      return;
    }
    fs_putc('o');
    fs_putc('k');
    fs_close();
    saved = sram_rd32(8);
    sram_wr32(8, sram_size());
    if (fs_open_write("OVR.TXT") == 0) {
      fs_close();
      kcon_puts("fstest: full-volume overwrite was allowed\n");
    } else if (fs_find("OVR.TXT") == 0) {
      kcon_puts("fstest: full-volume overwrite destroyed the old file\n");
    } else {
      kcon_puts("fstest: full-volume overwrite refused, file intact\n");
    }
    sram_wr32(8, saved);
    fs_delete("OVR.TXT");
  }

  /* Compaction: create A and B, delete A, compact, and demand (a) the space
   * really came back and (b) B's bytes survived the move byte-for-byte.
   * Then the torture case: arm the journal AS IF power died mid-move of B,
   * run recovery, and demand B is still intact -- the redo must be
   * idempotent whether the crash hit the copy, the commit, or the clear. */
  {
    uint32_t i, free0, free1;
    /* Each failure names its site. A compound "FAILED" already cost one
     * debugging session spent re-deriving which of five checks tripped. */
    const char *why = 0;
    fs_open_write("CA.TXT");
    for (i = 0; i < 512; i++) fs_putc((uint8_t)(i * 3u));
    fs_close();
    fs_open_write("CB.TXT");
    for (i = 0; i < 512; i++) fs_putc((uint8_t)(i * 7u));
    fs_close();
    free0 = fs_free();
    fs_delete("CA.TXT");
    if (fs_compact() < 0) why = "compact refused";
    free1 = fs_free();
    if (!why && free1 < free0 + 512) why = "space not reclaimed";
    if (!why) {
      const fs_entry_t *b = fs_find("CB.TXT");
      if (!b || b->size != 512) why = "CB lost by compact";
      else
        for (i = 0; i < 512; i++)
          if (fs_read_byte(b, i) != (uint8_t)(i * 7u)) {
            why = "CB corrupt after compact";
            break;
          }
    }
    /* Torture: re-arm the journal for CB as if the move died mid-copy. */
    if (!why) {
      const fs_entry_t *b = fs_find("CB.TXT");
      if (b) {
        sram_wr32(fs_journal_off() + 4u, b->off); /* dest = where it is */
        sram_wr32(fs_journal_off(), (uint32_t)b->slot + 1u);
        fs_compact_recover();             /* must be a harmless redo */
        fs_init();
        b = fs_find("CB.TXT");
        if (!b) why = "CB lost by recovery";
        else
          for (i = 0; i < 512; i++)
            if (fs_read_byte(b, i) != (uint8_t)(i * 7u)) {
              why = "CB corrupt after recovery";
              break;
            }
      }
    }
    /* The journal must be IDLE afterwards. An armed journal lives in
     * battery SRAM and outlives the run: it made this very test pass or
     * fail depending on what the previous one left behind. */
    if (!why && sram_rd32(fs_journal_off()) != 0) why = "journal not idle";
    fs_delete("CB.TXT");
    if (why) {
      kcon_puts("fstest: COMPACT FAILED: ");
      kcon_puts(why);
      kcon_putc('\n');
    } else {
      kcon_puts("fstest: compact reclaimed and survived recovery\n");
    }
  }
}

/* M10 step 1's proof: run two kernel workers under the NMI round-robin for
 * ~4 seconds. The workers never yield, never trap and never return, so BOTH
 * counters advancing is a fact only genuine preemption can produce — and
 * this line printing at all proves process 0 got the machine back. */
static void cmd_mtest(void) {
  uint32_t a, b;
  if (SNOS_EXEC_RUN) {
    kcon_puts("mtest: not while an application is running\n");
    return;
  }
  if (proc_state(1) != 0 || proc_state(2) != 0) {
    /* The workers forge frames into the process slots; stomping a live
     * background job's stack is not a diagnostic. */
    kcon_puts("mtest: not while background jobs exist (see jobs)\n");
    return;
  }
  kcon_puts("mtest: preempting two workers for ~4s...\n");
  proc_mtest_start(240u);
  while (proc_busy()) kbd_idle();
  a = proc_test_a_count();
  b = proc_test_b_count();
  kcon_puts("mtest: A=");
  kcon_puthex16((uint16_t)(a >> 16));
  kcon_puthex16((uint16_t)a);
  kcon_puts(" B=");
  kcon_puthex16((uint16_t)(b >> 16));
  kcon_puthex16((uint16_t)b);
  kcon_puts(a != 0 && b != 0 ? " both progressed OK\n"
                             : " STALLED - preemption is broken\n");
}

/* Ask the far end to prove it is there, rather than reporting the mailbox.
 *
 * net_up() only tests whether the *bridge* claimed the link. The daemon behind
 * it can be stopped, crashed or wedged and this still said "gateway up" — which
 * it cheerfully did during a session where every fetch was timing out. A status
 * line that is right only when nothing is wrong is worse than none, because it
 * is consulted precisely when something is. */
static void cmd_net_status(void) {
  uint16_t before, start, deadline, waited, rx0;

  if (!net_up()) {
    kcon_puts("no link (the bridge script is not running)\n");
    kcon_puts("(emulator: run build/snosbridge.lua; hardware: M7)\n");
    return;
  }

  before = net_stat_hello;
  rx0 = net_stat_rx_frames;
  if (net_hello() < 0) {
    kcon_puts("link up, but the send queue is full\n");
    return;
  }

  /* Three seconds, not one.
   *
   * A HELLO answers in about 7 frames on an idle link. After a 49 KB transfer
   * — a page in `web` — it was measured at 21 to 30, with the resend counter up
   * from 3 to 80: the reply queues behind whatever the link is still carrying.
   * One second was close enough to that to report "the gateway is NOT
   * answering" about a gateway that was working perfectly, which is a lie told
   * by the one command whose entire job is to be trusted about this. */
  start = snos_frames;
  deadline = (uint16_t)(start + 180u);
  while (net_stat_hello == before) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      /* Say what the link did while we waited. Frames arriving means the far
       * end is alive and merely slow or busy; none at all means nothing is
       * coming back, and those want different things done about them. */
      uint16_t moved = (uint16_t)(net_stat_rx_frames - rx0);
      kcon_puts("no answer in 3s. ");
      kcon_putu(moved);
      kcon_puts(" frames in, ");
      kcon_putu(net_stat_resent);
      kcon_puts(" resends\n");
      kcon_puts(moved ? "(the link is alive - the gateway is busy or wedged)\n"
                      : "(nothing is coming back - is tools/gateway.py running?)\n");
      return;
    }
    kbd_idle();
  }
  /* Against the recorded start, not deadline-minus-a-constant: the constant
   * was still 60 after the timeout above became 180, and every normal reply
   * printed as a ~65000-frame underflow. */
  waited = (uint16_t)(snos_frames - start);

  kcon_puts("gateway alive");
  if (net_text[0]) {
    /* The SNES has no address of its own, so anything it serves is reached at
     * the gateway's. This is where you look before pointing a browser at it. */
    kcon_puts(" @ ");
    kcon_puts(net_text);
  }
  kcon_puts(", v");
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

  /* M11: whether this gateway is a slirp box, and what our address is when
   * it is. The address is a constant of the design (ip.h), but printing it
   * here is what makes `net status` the one command that answers "what do I
   * ping / point a port-forward at". */
  if (net_ipmode()) {
    ip_uipstat_t u;
    kcon_puts("ip 10.0.2.15 via 10.0.2.2 (slirp), dns 10.0.2.3\n");
    kcon_puts("ip rx ");
    kcon_putu(ip_stat_rx);
    kcon_puts(", tx ");
    kcon_putu(ip_stat_tx);
    kcon_puts(", drop ");
    kcon_putu(ip_stat_drop);
    kcon_puts(", desync ");
    kcon_putu(ip_stat_desync);
    kcon_putc('\n');
    /* The stack's own view, which answers the other half of every network
     * question: the link moved N datagrams, and uIP made THIS of them. */
    ip_uipstat(&u);
    kcon_puts("uip ip ");
    kcon_putu(u.ip_recv);
    kcon_putc('/');
    kcon_putu(u.ip_sent);
    kcon_puts(" drop ");
    kcon_putu(u.ip_drop);
    kcon_puts(", icmp ");
    kcon_putu(u.icmp_recv);
    kcon_putc('/');
    kcon_putu(u.icmp_sent);
    kcon_puts("\nuip tcp rx ");
    kcon_putu(u.tcp_recv);
    kcon_puts(" drop ");
    kcon_putu(u.tcp_drop);
    kcon_puts(" rexmit ");
    kcon_putu(u.tcp_rexmit);
    kcon_puts(" rst ");
    kcon_putu(u.tcp_rst);
    kcon_puts(", udp drop ");
    kcon_putu(u.udp_drop);
    kcon_puts(", dns ");
    /* The resolver is a single shared slot, so "what is it doing" is a real
     * question when a connection will not start. */
    switch (ip_resolve_slot()) {
    case IP_RES_IDLE: kcon_puts("idle"); break;
    case IP_RES_BUSY: kcon_puts("BUSY"); break;
    case IP_RES_OK: kcon_puts("held"); break;
    default: kcon_puts("failed"); break;
    }
    kcon_putc('\n');
  } else {
    /* The daemon answered HELLO but never claimed NAT support. Since the
     * cutover there is no stream fallback: nothing network-shaped works
     * against it, and saying so here beats every app failing one by one. */
    kcon_puts("no ip - daemon did not claim NAT (v");
    kcon_putu(net_gw_version);
    kcon_puts("); nothing will connect\n");
  }
}

/* `ping A.B.C.D` — four real ICMP echo requests, composed on this machine.
 *
 * NOT the deleted `net ping`, and the difference is the milestone: that one
 * asked the gateway how the gateway felt over control records, this one puts
 * IPv4 packets this kernel built onto the link and parses the replies. */
static int parse_quad(const char *s, uint8_t out[4]) {
  int i;
  for (i = 0; i < 4; i++) {
    uint16_t v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
      v = (uint16_t)(v * 10u + (uint16_t)(*s - '0'));
      if (v > 255) return -1;
      s++;
      any = 1;
    }
    if (!any) return -1;
    out[i] = (uint8_t)v;
    if (i < 3) {
      if (*s != '.') return -1;
      s++;
    }
  }
  return (*s == 0 || *s == ' ') ? 0 : -1;
}

/* A dotted quad, or a name the console resolves itself. Returns 0 on
 * success and prints its own diagnosis on failure. */
static int host_addr(const char *s, uint8_t out[4]) {
  uint16_t deadline;
  int tag;
  if (parse_quad(s, out) == 0) return 0;
  if (!ip_up()) {
    kcon_puts("no ip: names need the console's own resolver\n");
    return -1;
  }
  tag = ip_resolve(s);
  if (tag < 0) {
    kcon_puts("resolver busy or the name is unusable\n");
    return -1;
  }
  deadline = (uint16_t)(snos_frames + 300u); /* 5 s: four tries, half a second
                                              * apart, plus the round trips */
  while (ip_resolve_state((uint8_t)tag) == IP_RES_BUSY) {
    if ((int16_t)(snos_frames - deadline) >= 0) break;
    if (snos_break_taken()) {
      /* Safe now that release is tag-checked: if the slot has moved on to
       * another asker, this is a no-op rather than a theft. */
      ip_resolve_release((uint8_t)tag);
      return -1;
    }
    kbd_idle();
  }
  if (ip_resolve_state((uint8_t)tag) != IP_RES_OK) {
    kcon_puts("cannot resolve ");
    kcon_puts(s);
    kcon_putc('\n');
    ip_resolve_release((uint8_t)tag);
    return -1;
  }
  ip_resolve_addr((uint8_t)tag, out);
  ip_resolve_release((uint8_t)tag);
  return 0;
}

static void put_quad(const uint8_t a[4]) {
  int i;
  for (i = 0; i < 4; i++) {
    kcon_putu(a[i]);
    if (i < 3) kcon_putc('.');
  }
}

static void cmd_resolve(const char *rest) {
  uint8_t a[4];
  if (rest[0] == 0) {
    kcon_puts("usage: resolve NAME\n");
    return;
  }
  if (host_addr(rest, a) < 0) return;
  kcon_puts(rest);
  kcon_puts(" is ");
  put_quad(a);
  kcon_putc('\n');
}

static void cmd_ping(const char *rest) {
  uint8_t dst[4];
  int i;
  if (rest[0] == 0) {
    kcon_puts("usage: ping HOST   (10.0.2.2 is the gateway)\n");
    return;
  }
  if (!ip_up()) {
    kcon_puts(net_up() ? "no ip - the daemon has not claimed NAT (net status)\n"
                       : "no link (the bridge script is not running)\n");
    return;
  }
  if (host_addr(rest, dst) < 0) return;
  for (i = 0; i < 4; i++) {
    uint16_t got = ip_ping_replies;
    uint16_t deadline;
    if (ip_ping_send(dst) < 0) {
      kcon_puts("send failed (queue full)\n");
      return;
    }
    /* Two seconds per echo: an emulated round trip is a handful of frames,
     * a real Internet host through the NAT can take dozens. */
    deadline = (uint16_t)(snos_frames + 120u);
    while (ip_ping_replies == got) {
      if ((int16_t)(snos_frames - deadline) >= 0) break;
      if (snos_break_taken()) return;
      kbd_idle();
    }
    if (ip_ping_replies != got) {
      kcon_puts("reply from ");
      put_quad(dst);
      kcon_puts(": seq=");
      kcon_putu(ip_ping_seq);
      kcon_puts(" time=");
      kcon_putu(ip_ping_rtt);
      kcon_puts(" frames\n");
    } else {
      kcon_puts("timeout (seq=");
      kcon_putu((uint16_t)i);
      kcon_puts(")\n");
    }
  }
}

/* `net test` — prove the whole path works, end to end: the console's own
 * resolver, its own SYN, its own request bytes, through the NAT to a real
 * host and back.
 *
 * Deliberately accepts ANY response, including an error page. The target is
 * behind Cloudflare and will refuse most non-browser requests, which is
 * fine: getting *anything* back means DNS resolved, TCP connected and the
 * link carried bytes in both directions. Asserting on the content would test
 * Cloudflare's mood rather than our networking. Plain port 80 — TLS is not a
 * thing this machine does, period-correctly. */
static const char NET_TEST_HOST[] = "rgsilva.com:80";

static uint8_t net_test_buf[64];

static void cmd_net_test(void) {
  static const char REQ[] = "GET / HTTP/1.0\r\nHost: rgsilva.com\r\n\r\n";
  int ch, st, sent = 0;
  uint32_t got = 0;
  uint16_t deadline;

  if (!ip_up()) {
    kcon_puts(net_up() ? "fail (no ip - is the daemon tools/gateway.py?)\n"
                       : "fail (no link - the bridge is not running)\n");
    return;
  }
  kcon_puts("testing ");
  kcon_puts(NET_TEST_HOST);
  kcon_puts("...\n");

  ch = net_connect(NET_TEST_HOST);
  if (ch < 0) {
    kcon_puts("fail (no free channel)\n");
    return;
  }

  deadline = (uint16_t)(snos_frames + 900u);
  while (net_state(ch) == NET_ST_OPENING) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("fail (connect timed out)\n");
      net_close(ch);
      return;
    }
    kbd_idle();
  }
  if (net_state(ch) != NET_ST_OPEN) {
    kcon_puts("fail (connect refused - DNS or the NAT)\n");
    net_close(ch);
    return;
  }

  while (REQ[sent]) {
    int n = net_write(ch, (const uint8_t *)REQ + sent,
                      (int)(sizeof REQ - 1 - (unsigned)sent));
    if (n > 0) sent += n;
    else kbd_idle();
    if ((int16_t)(snos_frames - deadline) >= 0) break;
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
  net_close(ch);

  if (got > 0) {
    kcon_puts("pass (");
    kcon_putu32(got);
    kcon_puts(" bytes back)\n");
  } else {
    kcon_puts("fail (connected, 0 bytes back)\n");
  }
}

/* Prove the upstream path end to end: open a plain TCP stream, WRITE a request
 * on it, and print what comes back. `get` cannot test this — the gateway
 * composes the request there, so every byte on the wire is still the gateway's.
 * Here the bytes are ours, which is the whole point of the stream API. */
static uint8_t send_buf[64];

static void cmd_send(char *hostport) {
  static const char REQ[] = "GET / HTTP/1.0\r\nHost: snos\r\nConnection: close\r\n\r\n";
  int ch, st, sent = 0, i;
  uint32_t got = 0;
  uint16_t deadline;

  if (*hostport == 0) {
    kcon_puts("usage: debug send HOST:PORT\n");
    return;
  }
  if (!ip_up()) {
    kcon_puts(net_up() ? "no ip - the daemon has not claimed NAT (net status)\n"
                       : "no link (the bridge script is not running)\n");
    return;
  }
  ch = net_connect(hostport);
  if (ch < 0) {
    kcon_puts("no free channel\n");
    return;
  }
  deadline = (uint16_t)(snos_frames + 1800u);
  while (net_state(ch) == NET_ST_OPENING) {
    if ((int16_t)(snos_frames - deadline) >= 0) {
      kcon_puts("connect timed out\n");
      net_close(ch);
      return;
    }
    kbd_idle();
  }
  if (net_state(ch) != NET_ST_OPEN) {
    kcon_puts("connect failed\n");
    net_close(ch);
    return;
  }

  /* Written in pieces against available space rather than assumed to fit: the
   * send queue drains at link speed, and a partial request is worse than none
   * because the peer parses what arrived. */
  while (REQ[sent]) {
    int room = net_wspace(ch);
    int n = 0;
    while (REQ[sent + n] && n < room && n < (int)sizeof send_buf) {
      send_buf[n] = (uint8_t)REQ[sent + n];
      n++;
    }
    if (n > 0) sent += net_write(ch, send_buf, n);
    else kbd_idle();
    if ((int16_t)(snos_frames - deadline) >= 0) break;
  }

  for (;;) {
    int n = net_read(ch, send_buf, (int)sizeof send_buf);
    if (n > 0) {
      for (i = 0; i < n; i++) kcon_putc(send_buf[i]);
      got += (uint32_t)n;
      if (got > 512) break;      /* a taste is enough to prove the path */
      continue;
    }
    st = net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    if ((int16_t)(snos_frames - deadline) >= 0) break;
    kbd_idle();
  }
  net_close(ch);
  kcon_puts("\n[");
  kcon_putu((uint16_t)got);
  kcon_puts(" bytes]\n");
}

static void cmd_net(char *rest) {
  char *what = rest;
  (void)split_word(rest);
  if (streq(what, "status")) cmd_net_status();
  else if (streq(what, "test")) cmd_net_test();
  else {
    kcon_puts("net status         the daemon, its address and counters\n");
    kcon_puts("net test           reach a known host to prove the path works\n");
  }
}

/* ---------------------------------------------------------------- shell */

/* Diagnostics live behind `debug` rather than cluttering the root namespace. */
static void cmd_debug(char *rest) {
  char *what = rest;
  const char *arg = split_word(rest);

  if (*what == 0) {
    kcon_puts("debug ver       version and ABI\n");
    kcon_puts("debug syscheck  exercise the COP syscall path\n");
    kcon_puts("debug fstest    verify SRAM addressing\n");
    kcon_puts("debug mtest     preempt two kernel workers (M10 step 1)\n");
    kcon_puts("debug send H:P  raw stream: connect, GET /, print\n");
    return;
  }
  if (streq(what, "send")) cmd_send((char *)arg);
  else if (streq(what, "ver")) cmd_ver();
  else if (streq(what, "syscheck")) cmd_syscheck();
  else if (streq(what, "fstest")) cmd_fstest();
  else if (streq(what, "mtest")) cmd_mtest();
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
  ip_init();

  kcon_puts("type 'help' for commands\n\n");
  for (;;) {
    char *rest;
    kcon_puts("> ");
    kbd_readline(line, (int)sizeof line);
    rest = split_word(line);

    if (line[0] == 0) continue;
    if (streq(line, "help")) cmd_help();
    else if (streq(line, "clear")) con_clear();
    else if (streq(line, "restart")) cmd_restart();
    else if (streq(line, "mem")) cmd_mem();
    else if (streq(line, "echo")) cmd_echo(rest);
    else if (streq(line, "apps")) cmd_apps();
    else if (streq(line, "run")) cmd_run(rest);
    else if (streq(line, "jobs")) cmd_jobs();
    else if (streq(line, "kill")) cmd_kill(rest);
    else if (streq(line, "fg")) cmd_fg(rest);
    else if (streq(line, "ls")) cmd_ls();
    else if (streq(line, "cat")) cmd_cat(rest);
    else if (streq(line, "write")) cmd_write(rest);
    else if (streq(line, "rm")) cmd_rm(rest);
    else if (streq(line, "compact")) cmd_compact();
    else if (streq(line, "net")) cmd_net(rest);
    else if (streq(line, "ping")) cmd_ping(rest);
    else if (streq(line, "resolve")) cmd_resolve(rest);
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
