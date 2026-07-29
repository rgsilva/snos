/* SNOS GET — fetch a URL to a file or to the console.
 *
 * This is an application rather than a shell builtin, and that is a deliberate
 * correction. Fetching is a tool, not a kernel service: the kernel already
 * provides the stream (net.c) and the filesystem, and everything else here is
 * user interface — argument parsing, a progress readout, a place to put the
 * bytes. None of that belongs in a shell that also has to fit in bank $00's
 * budget alongside the trap handlers.
 *
 *     get URL DEST
 *
 * DEST is mandatory. "-" prints to the console; anything else is a filename,
 * overwritten if it exists. Two commands (`get` printing, `download` saving)
 * meant no way to tell from a call site which behaviour you had.
 *
 * ------------------------------------------------------------------- images
 *
 * There is no `image` command and no flag. The gateway looks at the
 * Content-Type and converts a picture to Mode 3 tile data on its own, because
 * it is the only party that knows what came back and the only one that could
 * decode it anyway. Asking the user to declare it in advance was asking for
 * information the system already had.
 *
 * When that happens the saved name is forced to .IMG — the bytes are no longer
 * the resource that was requested, they are a rendition of it, and `view` finds
 * files by name. Printing them to the console is refused rather than sprayed.
 */
#include "snos_abi.h"

#define BUF 128
#define NAME_MAX 16

/* Conversion happens before the gateway answers, and quantising a large JPEG
 * is seconds of work on the far side — so the wait for a status has to be
 * generous. A plain fetch of a slow server can take just as long, so there is
 * no reason to distinguish. */
/* How long to wait for the gateway's answer, covering DNS, TCP, TLS, the
 * download and — for a picture — decoding and rescaling it.
 *
 * Instrumenting the gateway showed the whole sequence takes 0.3 s for a 960 KB
 * PNG, so thirty seconds is already a hundredfold margin. I had raised this to
 * two minutes on the theory that a slow conversion was causing the reported
 * timeouts; the measurement says otherwise, so it goes back. The wait being
 * interruptible and visible below is worth keeping on its own merits. */
#define OPEN_DEADLINE 1800u

static char args[SNOS_ARGV_MAX];
static char name[NAME_MAX + 1];
static uint8_t buf[BUF];

static void say(const char *s) { sys_puts(s); }

static void say_u32(uint32_t v) {
  /* sys_putu takes 16 bits; a download can exceed that. */
  char d[11];
  int n = 0;
  if (v == 0) {
    sys_putc('0');
    return;
  }
  while (v && n < 11) {
    d[n++] = (char)('0' + (uint16_t)(v % 10u));
    v /= 10u;
  }
  while (n) sys_putc((uint16_t)(unsigned char)d[--n]);
}

/* Split off the first word in place; returns the rest. */
static char *split(char *s) {
  while (*s == ' ') s++;
  while (*s && *s != ' ') s++;
  if (*s) {
    *s++ = 0;
    while (*s == ' ') s++;
  }
  return s;
}

/* Uppercase, because that is the convention on this volume: the ROM packer
 * emits uppercase and BASIC's SAVE writes uppercase. Matching is
 * case-insensitive so this is cosmetic, but a listing that mixes the two reads
 * like a bug. */
static void copy_name(const char *src, char *dst) {
  int i = 0;
  while (src[i] && i < NAME_MAX) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    dst[i] = c;
    i++;
  }
  dst[i] = 0;
}

static void force_img_ext(char *n) {
  int i = 0, dot = -1;
  while (n[i]) {
    if (n[i] == '.') dot = i;
    i++;
  }
  if (dot >= 0) i = dot;
  if (i > NAME_MAX - 4) i = NAME_MAX - 4;
  n[i++] = '.';
  n[i++] = 'I';
  n[i++] = 'M';
  n[i++] = 'G';
  n[i] = 0;
}

/* Last path segment of a URL, for when no name is wanted from it. Kept because
 * a converted image always needs a name even if DEST said something else. */
static void name_from_url(const char *url, char *out) {
  const char *p = url, *seg = url;
  int i = 0;

  while (*p) { /* skip the scheme so "//" is not read as a path separator */
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
    else if (*p == '?' || *p == '#') break;
  }
  for (; seg < p && *seg && i < NAME_MAX; seg++) {
    char c = *seg;
    if (c == '?' || c == '#') break;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    out[i++] = c;
  }
  out[i] = 0;
  if (i == 0) {
    const char *d = "INDEX.HTM";
    while (*d) out[i++] = *d++;
    out[i] = 0;
  }
}

int main(void) {
  char *url, *dest;
  uint16_t ch, st, http, is_img;
  uint16_t deadline;
  uint32_t got = 0, budget, dots = 0;
  int to_console, aborted = 0, full = 0;
  snos_getargs(args, sizeof args);
  url = args;
  dest = split(args);

  if (*url == 0 || *dest == 0) {
    say("usage: get URL DEST\n");
    say("       DEST of - prints to the console\n");
    return 1;
  }
  if (!sys_net_up()) {
    say("no gateway attached\n");
    return 1;
  }

  to_console = (dest[0] == '-' && dest[1] == 0);
  ch = sys_net_fetch(url);
  if (ch == SYS_ERR) {
    say("fetch failed: no free channel\n");
    return 1;
  }
  deadline = (uint16_t)(sys_frames() + OPEN_DEADLINE);
  {
    uint16_t next_tick = (uint16_t)(sys_frames() + 120u);
    uint16_t frames0 = sys_net_stats();
    for (;;) {
      st = sys_net_state(ch);
      if (st != NET_ST_OPENING) break;
      if ((int16_t)(sys_frames() - deadline) >= 0) {
        /* Say which half stalled. A gateway log showing a clean fetch next to a
         * SNES showing a timeout is not a contradiction — it means the reply
         * never crossed the link — but the bare message left both ends equally
         * suspect, and I burned a lot of time guessing between them. If frames
         * moved during the wait the link is alive and the answer went missing;
         * if they did not, the bridge is not running. */
        uint16_t moved = (uint16_t)(sys_net_stats() - frames0);
        say("\ntimeout waiting for the gateway\n");
        sys_putu(moved);
        say(" frames in, ");
        sys_putu(SNOS_ARGS.p0);
        say(" resends\n");
        say(moved ? "link is alive - the reply never crossed it\n"
                  : "nothing came back - check the bridge and the daemon\n");
        sys_net_close(ch);
        return 1;
      }
      /* Any key abandons the wait. Converting a large picture takes long enough
       * that a silent, uninterruptible pause is indistinguishable from a hang —
       * which is exactly what it looked like. */
      if (sys_pollkey() != SYS_ERR) {
        say("\n[cancelled]\n");
        sys_net_close(ch);
        return 1;
      }
      /* A dot every two seconds, so the wait is visibly alive. */
      if ((int16_t)(sys_frames() - next_tick) >= 0) {
        next_tick = (uint16_t)(sys_frames() + 120u);
        sys_putc('.');
      }
      sys_yield();
    }
    if ((int16_t)(sys_frames() - (uint16_t)(deadline - OPEN_DEADLINE) - 120) >= 0)
      sys_putc('\n'); /* close the dotted line if we printed any */
  }

  /* sys_net_state leaves the status in p0 and the image flag in p1. Read them
   * before anything else calls a syscall and overwrites the block. */
  http = SNOS_ARGS.p0;
  is_img = SNOS_ARGS.p1;

  if (st == NET_ST_ERROR) {
    say("gateway error ");
    sys_putu(http);
    sys_putc('\n');
    sys_net_close(ch);
    return 1;
  }

  if (to_console) {
    if (is_img) {
      /* Tile data is not text. Refusing is more useful than 43 KB of noise. */
      say("that is a picture - give it a filename, then: view NAME\n");
      sys_net_close(ch);
      return 1;
    }
    say("HTTP ");
    sys_putu(http);
    sys_putc('\n');
    for (;;) {
      uint16_t n = sys_net_read(ch, buf, BUF);
      uint16_t i;
      if (n > 0 && n != SYS_ERR) {
        for (i = 0; i < n; i++) sys_putc(buf[i]);
        got += n;
        continue;
      }
      if (sys_pollkey() != SYS_ERR) {
        say("\n[aborted]\n");
        break;
      }
      st = sys_net_state(ch);
      if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
      sys_yield();
    }
    say("\n[");
    say_u32(got);
    say(" bytes]\n");
    sys_net_close(ch);
    return 0;
  }

  if (http != 200) {
    say("HTTP ");
    sys_putu(http);
    say(" - not saved\n");
    sys_net_close(ch);
    return 1;
  }

  copy_name(dest, name);
  if (name[0] == 0) name_from_url(url, name);
  if (is_img) force_img_ext(name);

  /* Refuse before transferring rather than after filling the volume. */
  budget = (uint32_t)sys_fs_free() << 10;
  if (budget < 1024ul) {
    say("not enough room on the SRAM volume\n");
    sys_net_close(ch);
    return 1;
  }

  snos_set_ptr(name);
  if (sys_open(1) == SYS_ERR) {
    say("cannot create ");
    say(name);
    sys_putc('\n');
    sys_net_close(ch);
    return 1;
  }

  say("-> ");
  say(name);
  sys_putc('\n');

  for (;;) {
    uint16_t n, i;

    /* Poll for an abort on EVERY pass, not only when the ring runs dry. The
     * obvious placement, beside the state check below, is reached only when no
     * data is waiting — so a transfer that streams steadily becomes
     * uninterruptible exactly when it is longest, which is the case this
     * exists for. */
    if (sys_pollkey() != SYS_ERR) {
      aborted = 1;
      break;
    }

    n = sys_net_read(ch, buf, BUF);
    if (n > 0 && n != SYS_ERR) {
      if ((uint32_t)n > budget) {
        full = 1;
        break;
      }
      for (i = 0; i < n; i++) {
        if (sys_putc_f(buf[i]) == SYS_ERR) {
          full = 1;
          break;
        }
      }
      if (full) break;
      got += n;
      budget -= n;
      /* One dot per kilobyte. Not a repainted byte count: redrawing costs a
       * vblank each time, and the link is skipped on any frame the console is
       * still dirty — so a livelier readout literally slows the transfer it is
       * reporting on. */
      while (dots < (got >> 10)) {
        sys_putc('.');
        dots++;
      }
      continue;
    }
    st = sys_net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    sys_yield();
  }

  sys_putc('\n');
  sys_close();

  if (full) say("[volume full - partial file kept]\n");
  else if (aborted) say("[aborted - partial file kept]\n");
  else {
    say("saved ");
    say(name);
    say(", ");
    say_u32(got);
    say(" bytes\n");
  }
  sys_net_close(ch);
  return 0;
}
