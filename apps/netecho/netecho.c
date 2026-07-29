/* SNOS NETECHO — a TCP echo server, as small as one can be.
 *
 *     netecho [PORT]        default 7777 (7 is RFC 862's, but needs root)
 *
 * It exists to prove the two things `get` cannot: that the SNES can ACCEPT a
 * connection, and that it can SEND bytes it composed itself. Everything the
 * console did before this was a request it made and a reply it read.
 *
 * Deliberately trivial — no protocol, no parsing, no state. Whatever arrives
 * goes straight back, so anything wrong is the stream API's fault and not this
 * program's. `httpd` is the same machinery with a parser on top.
 *
 * One connection at a time. There are three channels in total and the link
 * moves ~615 B/s; a queue of parallel clients would be a queue of timeouts.
 */
#include "snos_abi.h"

#define BUF 64

static char args[SNOS_ARGV_MAX];
static uint8_t buf[BUF];

static uint16_t parse_port(const char *s) {
  uint16_t v = 0;
  int any = 0;
  while (*s == ' ') s++;
  while (*s >= '0' && *s <= '9') {
    v = (uint16_t)(v * 10u + (uint16_t)(*s - '0'));
    s++;
    any = 1;
  }
  return any ? v : 0;
}

int main(void) {
  uint16_t port, ch, st;
  uint32_t served = 0;

  snos_getargs(args, sizeof args);
  port = parse_port(args);
  if (port == 0) port = 7777;

  if (!sys_net_up()) {
    sys_puts("no gateway attached\n");
    return 1;
  }

  ch = sys_net_listen(port);
  if (ch == SYS_ERR) {
    sys_puts("cannot listen (no free channel, or port in use)\n");
    return 1;
  }

  sys_puts("echo on port ");
  sys_putu(port);
  sys_puts(" - 'net status' shows the address\n");
  sys_puts("any key stops\n");

  for (;;) {
    st = sys_net_state(ch);

    if (st == NET_ST_OPEN || st == NET_ST_EOF) {
      uint16_t n = sys_net_read(ch, buf, BUF);
      if (n > 0 && n != SYS_ERR) {
        uint16_t sent = 0;
        /* Push it back out against available space rather than assuming it
         * fits. The send queue drains at link speed, so a burst larger than it
         * would otherwise be silently truncated — and a truncated echo is a
         * wrong answer rather than a slow one. */
        while (sent < n) {
          uint16_t took = sys_net_write(ch, (const char *)(buf + sent),
                                        (uint16_t)(n - sent));
          if (took == 0) {
            uint16_t w = sys_net_state(ch);
            if (w != NET_ST_OPEN && w != NET_ST_EOF) goto done;
            if (sys_pollkey() != SYS_ERR) goto done;
            sys_yield();
            continue;
          }
          sent = (uint16_t)(sent + took);
        }
        served += n;
        continue;             /* drain hard while there is anything to drain */
      }

      /* EOF means the client half-closed: it has said everything it intends to
       * and is waiting to read. Everything it sent has now been echoed, so end
       * this connection and go back to listening. sys_net_hangup refuses while
       * bytes are still on their way out, which is why this retries rather than
       * assuming it worked. */
      if (st == NET_ST_EOF) {
        if (sys_net_hangup(ch) != SYS_ERR) {
          sys_puts("client served, ");
          sys_putu((uint16_t)served);
          sys_puts(" bytes so far\n");
        }
        if (sys_pollkey() != SYS_ERR) break;
        sys_yield();
        continue;
      }
    } else if (st == NET_ST_ERROR) {
      sys_puts("listen failed - is the port already in use?\n");
      break;
    }

    if (sys_pollkey() != SYS_ERR) break;
    sys_yield();
  }

done:
  sys_net_close(ch);
  sys_puts("\nechoed ");
  sys_putu((uint16_t)served);
  sys_puts(" bytes\n");
  return 0;
}
