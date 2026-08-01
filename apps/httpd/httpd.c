/* SNOS HTTPD — a Super Nintendo serving web pages.
 *
 *     httpd [PORT]          default 8080
 *
 * 8080 and not 80, because the gateway binds the socket and ports below 1024
 * need root — defaulting to 80 would mean every first run failing with a
 * permission error that has nothing to do with the SNES. `httpd 80` works if
 * the daemon was started with privileges.
 *
 * GET only, HTTP/1.1, one connection at a time. Files come from the same
 * volume everything else uses, so `edit INDEX.HTM` and then reloading the page
 * is the whole authoring workflow.
 *
 * Deliberately not clever. No keep-alive: every response says Connection:
 * close and the socket ends the body, which removes an entire class of
 * framing bug and costs nothing at these sizes. No chunked encoding, no gzip,
 * no ranges, no conditional requests.
 *
 * What to expect: the link moves ~615 B/s, so a 4 KB page takes a few
 * seconds. curl and a phone fetching one page work fine. A desktop browser
 * opens six connections at once and will look broken — serving is one
 * connection at a time by design (a second SYN is refused while one is being
 * served; the loop re-listens between requests), not a bug in here.
 */
#include "snos_abi.h"
#include "snoslib.h"

#define BUF 64
#define NAME_MAX 16
#define REQ_MAX 128

static char args[SNOS_ARGV_MAX];
static uint8_t buf[BUF];
static char req[REQ_MAX + 1];
static char name[NAME_MAX + 1];

static int send_str(uint16_t ch, const char *s) {
  uint16_t n = 0;
  while (s[n]) n++;
  return sl_send_all(ch, s, n);
}

static void send_u32(uint16_t ch, uint32_t v) {
  char d[11];
  int n = 0;
  if (v == 0) {
    send_str(ch, "0");
    return;
  }
  while (v && n < 11) {
    d[n++] = (char)('0' + (uint16_t)(v % 10u));
    v /= 10u;
  }
  while (n) {
    char c = d[--n];
    sl_send_all(ch, &c, 1);
  }
}

/* Content-Type from the extension. A browser that guesses wrong renders HTML as
 * text or refuses an image, so this is not cosmetic. */
static const char *mime_for(const char *n) {
  int i = 0, dot = -1;
  while (n[i]) {
    if (n[i] == '.') dot = i;
    i++;
  }
  if (dot < 0) return "application/octet-stream";
  {
    const char *e = n + dot + 1;
    if ((e[0] == 'H' && e[1] == 'T' && e[2] == 'M')) return "text/html";
    if ((e[0] == 'T' && e[1] == 'X' && e[2] == 'T')) return "text/plain";
    if ((e[0] == 'C' && e[1] == 'S' && e[2] == 'S')) return "text/css";
    if ((e[0] == 'J' && e[1] == 'S')) return "text/javascript";
    if ((e[0] == 'P' && e[1] == 'N' && e[2] == 'G')) return "image/png";
    if ((e[0] == 'G' && e[1] == 'I' && e[2] == 'F')) return "image/gif";
  }
  return "application/octet-stream";
}

/* "GET /path HTTP/1.1" -> a volume filename.
 *
 * Percent-escapes and query strings are cut rather than decoded: nothing on
 * this volume has a name that needs either, and a decoder is a place for bugs
 * to live. A path with '/' or '..' in it is refused outright — the volume is
 * flat, so any separator at all is either a mistake or an attempt to leave. */
static int name_from_path(const char *p, char *out) {
  int i = 0;
  while (*p == '/') p++;
  if (*p == 0 || *p == ' ') {
    const char *d = "INDEX.HTM";
    while (*d) out[i++] = *d++;
    out[i] = 0;
    return 1;
  }
  while (*p && *p != ' ' && *p != '?' && *p != '#' && i < NAME_MAX) {
    char c = *p++;
    if (c == '/' || c == '%' || c == '\\') return 0;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    out[i++] = c;
  }
  out[i] = 0;
  /* A truncated name would silently serve the wrong file. */
  if (*p && *p != ' ' && *p != '?' && *p != '#') return 0;
  return i > 0;
}

static void reply_error(uint16_t ch, const char *status, const char *body) {
  send_str(ch, "HTTP/1.1 ");
  send_str(ch, status);
  send_str(ch, "\r\nContent-Type: text/html\r\nConnection: close\r\n"
                "Content-Length: ");
  {
    uint16_t n = 0;
    while (body[n]) n++;
    send_u32(ch, n);
  }
  send_str(ch, "\r\n\r\n");
  send_str(ch, body);
}

/* Read the request line. Everything after it is discarded: this serves GET, and
 * headers cannot change the answer when there is no keep-alive, no compression
 * and no conditional requests. */
static int read_request(uint16_t ch, uint16_t deadline) {
  int len = 0;
  for (;;) {
    uint16_t n = sys_net_read(ch, buf, 1);
    if (n == 1) {
      char c = (char)buf[0];
      if (c == '\n') {
        req[len] = 0;
        return len > 0;
      }
      if (c != '\r' && len < REQ_MAX) req[len++] = c;
      continue;
    }
    if (sys_net_state(ch) != NET_ST_OPEN) return 0;
    if ((int16_t)(sys_frames() - deadline) >= 0) return 0;
    sys_yield();
  }
}

int main(void) {
  uint16_t port, ch, st;
  uint32_t served = 0;

  snos_getargs(args, sizeof args);
  port = sl_parse_port(args, 8080);

  if (!sys_net_up()) {
    sys_puts("no gateway attached\n");
    return 1;
  }
  ch = sys_net_listen(port);
  if (ch == SYS_ERR) {
    sys_puts("cannot listen (no free channel, or port in use)\n");
    return 1;
  }

  sys_puts("serving port ");
  sys_putu(port);
  sys_puts(" - 'net status' shows the address\n");
  sys_puts("any key stops\n");

  for (;;) {
    st = sys_net_state(ch);

    /* EOF as well as OPEN: an HTTP client half-closes after its request and
     * then waits, so the request may well be complete on a channel that is
     * already reading end-of-stream. */
    if (st == NET_ST_OPEN || st == NET_ST_EOF) {
      uint16_t deadline = (uint16_t)(sys_frames() + 900u);
      if (read_request(ch, deadline)) {
        if (req[0] != 'G' || req[1] != 'E' || req[2] != 'T' || req[3] != ' ') {
          sys_puts("! non-GET refused\n");
          reply_error(ch, "405 Method Not Allowed",
                      "<h1>405</h1>SNOS serves GET only.\n");
        } else if (!name_from_path(req + 4, name)) {
          sys_puts("! bad path refused\n");
          reply_error(ch, "400 Bad Request",
                      "<h1>400</h1>The volume is flat: no paths.\n");
        } else {
          snos_set_ptr(name);
          if (sys_open(0) == SYS_ERR) {
            sys_puts("404 ");
            sys_puts(name);
            sys_putc('\n');
            reply_error(ch, "404 Not Found", "<h1>404</h1>No such file.\n");
          } else {
            uint32_t size = sys_fsize();
            sys_puts("200 ");
            sys_puts(name);
            sys_putc('\n');
            /* No Content-Length, deliberately: sys_fsize returns the LOW 16
             * BITS (snos_abi.h), so a >=64 KB file would advertise a wrapped
             * length and the client would truncate or hang. Connection:
             * close means the socket end delimits the body — correct
             * HTTP/1.1, and correct at every size. */
            (void)size;
            send_str(ch, "HTTP/1.1 200 OK\r\nContent-Type: ");
            send_str(ch, mime_for(name));
            send_str(ch, "\r\nConnection: close\r\n\r\n");
            for (;;) {
              uint16_t n = sys_read_f(buf, BUF);
              if (n == 0 || n == SYS_ERR) break;
              if (!sl_send_all(ch, (const char *)buf, n)) break;
              served += n;
            }
            sys_close();
          }
        }
      }
      /* Connection: close, so the response ends when the socket does. Hang up
       * rather than just closing: sys_net_hangup refuses while bytes are still
       * queued, so retrying here is what guarantees the client sees the whole
       * page instead of however much had drained when we let go. */
      {
        /* Bounded: a client that stops draining must not pin a background
         * server forever (pollkey never answers back there). ~5 s, then
         * drop the connection and rebind. */
        uint16_t give_up = (uint16_t)(sys_frames() + 300u);
        while (sys_net_hangup(ch) == SYS_ERR) {
          if (sys_net_state(ch) == NET_ST_ERROR) break;
          if ((int16_t)(sys_frames() - give_up) >= 0) {
            sys_net_close(ch);
            ch = sys_net_listen(port);
            if (ch == SYS_ERR) goto done;
            break;
          }
          if (sys_pollkey() != SYS_ERR) goto done;
          sys_yield();
        }
      }
      continue;
    }

    if (st == NET_ST_ERROR) {
      sys_puts(served ? "connection error\n"
                      : "listen failed - is the port already in use?\n");
      break;
    }
    if (sys_pollkey() != SYS_ERR) break;
    sys_yield();
  }

done:
  sys_net_close(ch);
  sys_puts("\nserved ");
  sl_putu32(served);
  sys_puts(" bytes\n");
  return 0;
}
