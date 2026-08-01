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
 * ------------------------------------------------------------ the request
 *
 * Since the uIP cutover this is a real HTTP client: the request line below
 * goes out over the console's own TCP, the status line and headers come back
 * as bytes in the stream, and this file parses them. HTTP/1.0 on purpose —
 * the reply is "everything until the server closes", so there is no chunked
 * encoding and no Content-Length arithmetic, which is exactly the protocol a
 * machine this size wants. It is also period-correct to the month.
 *
 * https:// is refused, honestly: TLS does not fit this machine and never
 * will (PLAN.md M11 — "back in the day this wouldn't exist anyway"). The
 * gateway that once terminated it for us is gone; anything that needs it can
 * be fronted by a plain-HTTP service (M12).
 *
 * Images: the old gateway converted pictures to Mode 3 tiles in flight. No
 * conversion happens anywhere now — what the server sends is what lands. A
 * file that already IS tile data (.IMG) downloads byte-for-byte and `view`
 * shows it, which is the M12 shape: conversion as an ordinary HTTP service.
 */
#include "snos_abi.h"
#include "snoslib.h"

#define BUF 128
#define NAME_MAX 16
#define HOST_MAX 48
#define PATH_MAX 128

/* How long to wait for the connection: DNS on the console, the SYN through
 * the NAT, and a slow server's accept. A real Internet host through the
 * daemon answers in well under a second; thirty is the "something is wrong,
 * tell me about it" bound, and the wait is visible and interruptible. */
#define OPEN_DEADLINE 1800u

static char args[SNOS_ARGV_MAX];
static char name[NAME_MAX + 1];
static char hostport[HOST_MAX + 8];
static char host[HOST_MAX + 1];
static char path[PATH_MAX + 1];
static uint8_t buf[BUF];

static void say(const char *s) { sys_puts(s); }

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

static int starts_with(const char *s, const char *p) {
  int i = 0;
  while (p[i]) {
    if (s[i] != p[i]) return 0;
    i++;
  }
  return 1;
}

/* url -> host[:port] and /path. Returns 0 on success, -1 on a URL this
 * machine cannot speak. The scheme is optional; the path defaults to "/". */
static int parse_url(const char *url) {
  int i = 0, h = 0, p = 0;
  if (starts_with(url, "https://")) return -1; /* refused, loudly, above */
  if (starts_with(url, "http://")) i = 7;
  else if (url[0] == 0) return -1;
  while (url[i] && url[i] != '/' && h < HOST_MAX) host[h++] = url[i++];
  host[h] = 0;
  if (h == 0 || url[i] == 0) {
    path[0] = '/';
    path[1] = 0;
  } else {
    while (url[i] && p < PATH_MAX) path[p++] = url[i++];
    path[p] = 0;
    if (url[i]) return -1; /* path longer than this machine cares to carry */
  }
  if (h == 0) return -1;
  /* :80 unless the URL named a port of its own. */
  sl_hostport(hostport, (int)sizeof hostport, host, ":80");
  return 0;
}

/* Trim a ":port" suffix off `host` for the Host: header — the header wants
 * the name, and the port only when it is not the default. Keeping the port
 * in would usually work; keeping it out always works at our scale. */
static void host_only(char *s) {
  int i = 0;
  while (s[i] && s[i] != ':') i++;
  s[i] = 0;
}

int main(void) {
  char *url, *dest;
  uint16_t ch, st;
  uint16_t deadline;
  uint32_t got = 0, budget = 0, dots = 0;
  uint16_t http = 0;
  int to_console, to_file_open = 0, aborted = 0, full = 0;
  /* Response parsing: 0 = status line, 1 = headers, 2 = body. */
  int phase = 0, digits = 0, nl = 0, seen_sp = 0;

  snos_getargs(args, sizeof args);
  url = args;
  dest = split(args);

  if (*url == 0 || *dest == 0) {
    say("usage: get URL DEST\n");
    say("       DEST of - prints to the console\n");
    return 1;
  }
  if (!sys_net_up()) {
    say("no link attached\n");
    return 1;
  }
  if (starts_with(url, "https://")) {
    say("https needs TLS, which this machine does not do\n");
    say("(period-correct; try http://, or front it with a service)\n");
    return 1;
  }
  if (parse_url(url) < 0) {
    say("cannot parse that URL (http://host[:port]/path)\n");
    return 1;
  }

  to_console = (dest[0] == '-' && dest[1] == 0);
  ch = sys_net_connect(hostport);
  if (ch == SYS_ERR) {
    say("connect failed: no free channel or no ip (net status)\n");
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
        /* Say which half stalled. If frames moved during the wait the link is
         * alive and the answer went missing (DNS, or the far host); if they
         * did not, the bridge is not running. */
        uint16_t moved = (uint16_t)(sys_net_stats() - frames0);
        /* Latched NOW: say() marshals its string through the same param
         * block (snos_set_ptr writes p0), so reading p0 after a print
         * reports the string literal's address as the resend count -
         * garbage in the one diagnostic this branch exists for. */
        uint16_t resends = SNOS_ARGS.p0;
        say("\ntimeout waiting for the connection\n");
        sys_putu(moved);
        say(" frames in, ");
        sys_putu(resends);
        say(" resends\n");
        say(moved ? "link is alive - DNS or the far host never answered\n"
                  : "nothing came back - check the bridge and the daemon\n");
        sys_net_close(ch);
        return 1;
      }
      /* Any key abandons the wait: a silent, uninterruptible pause is
       * indistinguishable from a hang, which is what it used to be. */
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

  if (st != NET_ST_OPEN) {
    say("connect failed (DNS, or the host refused)\n");
    sys_net_close(ch);
    return 1;
  }

  /* The request. Bytes this machine composed, on a socket this machine owns —
   * paced against the send queue, because a partial request is worse than
   * none: the server parses what arrived. */
  {
    const char *part[6];
    int pi;
    host_only(host);
    part[0] = "GET ";
    part[1] = path;
    part[2] = " HTTP/1.0\r\nHost: ";
    part[3] = host;
    part[4] = "\r\n\r\n";
    part[5] = 0;
    for (pi = 0; part[pi]; pi++) {
      uint16_t n = 0;
      while (part[pi][n]) n++;
      if (!sl_send_all(ch, part[pi], n)) {
        say("the connection died mid-request\n");
        sys_net_close(ch);
        return 1;
      }
    }
  }

  /* The response: status line, headers, body — a state machine over the
   * stream, because that is all an HTTP/1.0 response is. The body ends when
   * the server closes, which arrives here as EOF. */
  for (;;) {
    uint16_t n, i;

    /* Poll for an abort on EVERY pass, not only when the ring runs dry: a
     * transfer that streams steadily is uninterruptible exactly when it is
     * longest otherwise. */
    if (sys_pollkey() != SYS_ERR) {
      aborted = 1;
      break;
    }

    n = sys_net_read(ch, buf, BUF);
    if (n > 0 && n != SYS_ERR) {
      for (i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (phase == 0) {
          /* "HTTP/1.x NNN ...": the code is the digits after the first
           * space — collecting the FIRST digit run would read the 1 and 0
           * out of "HTTP/1.0" and report status 102, which is exactly what
           * the first run of this parser did. */
          if (c == ' ' && digits == 0) {
            seen_sp = 1;
          } else if (seen_sp && c >= '0' && c <= '9' && digits < 3) {
            http = (uint16_t)(http * 10u + (c - '0'));
            digits++;
          } else if (c == '\n') {
            phase = 1;
            nl = 1;
          }
          continue;
        }
        if (phase == 1) {
          /* Headers end at the first empty line. */
          if (c == '\n') {
            nl++;
            if (nl >= 2) {
              phase = 2;
              /* Decide the sink now that the status is known. */
              if (!to_console) {
                if (http != 200) {
                  say("HTTP ");
                  sys_putu(http);
                  say(" - not saved\n");
                  sys_net_close(ch);
                  return 1;
                }
                copy_name(dest, name);
                /* Refuse before transferring rather than after filling the
                 * volume. */
                budget = (uint32_t)sys_fs_free() << 10;
                if (budget < 1024ul) {
                  say("not enough room on the SRAM volume (");
                  sl_putu32(budget >> 10);
                  say(" KB free)\n");
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
                to_file_open = 1;
                say("-> ");
                say(name);
                sys_putc('\n');
              } else {
                say("HTTP ");
                sys_putu(http);
                sys_putc('\n');
              }
            }
          } else if (c != '\r') {
            nl = 0;
          }
          continue;
        }
        /* Body. */
        if (to_console) {
          sys_putc((char)c);
          got++;
        } else {
          if (budget == 0 || sys_putc_f(c) == SYS_ERR) {
            full = 1;
            break;
          }
          budget--;
          got++;
          /* One dot per kilobyte — a livelier readout costs vblanks and
           * literally slows the transfer it reports on. */
          while (dots < (got >> 10)) {
            sys_putc('.');
            dots++;
          }
        }
      }
      if (full) break;
      continue;
    }
    st = sys_net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    sys_yield();
  }

  if (to_file_open) {
    sys_putc('\n');
    sys_close();
  }
  sys_net_close(ch);

  if (phase < 2) {
    say(aborted ? "[cancelled]\n"
                : "no response - the server closed before answering\n");
    return 1;
  }
  if (to_console) {
    say("\n[");
    sl_putu32(got);
    say(" bytes]\n");
    return 0;
  }
  if (full) say("[volume full - partial file kept]\n");
  else if (aborted) say("[aborted - partial file kept]\n");
  else {
    say("saved ");
    say(name);
    say(", ");
    sl_putu32(got);
    say(" bytes\n");
  }
  return 0;
}
