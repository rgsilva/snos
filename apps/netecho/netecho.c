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
#include "snoslib.h"

#define BUF 64

static char args[SNOS_ARGV_MAX];
static uint8_t buf[BUF];

int main(void) {
  uint16_t port, ch, st;
  uint16_t prev_st = 0;
  uint32_t served = 0;

  snos_getargs(args, sizeof args);
  port = sl_parse_port(args, 7777);

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

    /* A client connected. If we are backgrounded, ask for eyes: the kernel
     * shows the attention cell and clears it when we are foregrounded. From
     * the foreground the call is a no-op. */
    /* Any entry into OPEN is a fresh client — including EOF->LISTEN->OPEN
     * inside one polling gap, which the old prev!=EOF exclusion wrongly
     * suppressed. */
    if (st == NET_ST_OPEN && prev_st != NET_ST_OPEN)
      sys_notify();
    prev_st = st;

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
        /* Bounded like irc's drain: hangup refuses while bytes are unsent,
         * and a client that stops draining would otherwise pin a BACKGROUND
         * server forever — pollkey never answers back there. Give it ~5 s,
         * then drop the connection the hard way and return to listening. */
        uint16_t give_up = (uint16_t)(sys_frames() + 300u);
        for (;;) {
          if (sys_net_hangup(ch) != SYS_ERR) {
            sys_puts("client served, ");
            sl_putu32(served);
            sys_puts(" bytes so far\n");
            break;
          }
          if ((int16_t)(sys_frames() - give_up) >= 0) {
            sys_net_close(ch);
            ch = sys_net_listen(port);
            if (ch == SYS_ERR) goto done;
            break;
          }
          if (sys_pollkey() != SYS_ERR) goto done;
          sys_yield();
        }
        if (sys_pollkey() != SYS_ERR) break;
        sys_yield();
        continue;
      }
    } else if (st == NET_ST_ERROR) {
      /* Before the first byte this is almost always the bind; after it, it
       * is the connection dying — two different debugging sessions. */
      sys_puts(served ? "connection error\n"
                      : "listen failed - is the port already in use?\n");
      break;
    }

    if (sys_pollkey() != SYS_ERR) break;
    sys_yield();
  }

done:
  sys_net_close(ch);
  sys_puts("\nechoed ");
  sl_putu32(served);
  sys_puts(" bytes\n");
  return 0;
}
