/* The shell supplies the gateway address; the application owns the buffers.
 * Two connections in the duplex phase keep TCP's per-stream stop-and-wait
 * from imposing an extra dependency between the two directions. */
#include "snos_abi.h"
#include "snoslib.h"

#define WAIT 900u
#define BUF 512

static char args[SNOS_ARGV_MAX];
static char host[SNOS_ARGV_MAX];
static char request[24];
static char reply[24];
static char data[BUF];
static char incoming[BUF];
static uint16_t duration, down, up;
static uint32_t received, sent, remote;
static const char *failure;

static int expired(uint16_t start, uint16_t frames) {
  return (uint16_t)(sys_frames() - start) >= frames;
}

static int cancelled(void) {
  if (sys_pollkey() == SYS_ERR) return 0;
  failure = "cancelled";
  return 1;
}

static int alive(uint16_t ch) {
  uint16_t st = sys_net_state(ch);
  return st == NET_ST_OPEN || st == NET_ST_OPENING;
}

/* Control traffic has a deadline too: a peer that accepts and then stops
 * reading must not turn a fixed-duration test into an endless write. */
static int send_text(uint16_t ch, const char *s) {
  uint16_t start = sys_frames();
  while (*s) {
    uint16_t n = sys_net_write(ch, s, 1);
    if (n == 1) s++;
    else {
      if (!alive(ch) || expired(start, WAIT) || cancelled()) return 0;
      sys_yield();
    }
  }
  return 1;
}

static int read_line(uint16_t ch) {
  uint16_t i = 0, start = sys_frames();
  while (i + 1 < sizeof reply) {
    uint16_t n = sys_net_read(ch, reply + i, 1);
    if (n == 1) {
      if (reply[i] == '\n') {
        reply[i] = 0;
        return 1;
      }
      i++;
    } else {
      if (!alive(ch) || expired(start, WAIT) || cancelled()) return 0;
      sys_yield();
    }
  }
  failure = "bad peer reply";
  return 0;
}

/* Both sockets are ready before either gets GO. The receiving end owns
 * each measurement window: sys_frames here, monotonic time at the server.
 * Upload counts come from the peer, never from bytes queued in our ring. */
static uint16_t prepare(int upload) {
  uint16_t ch, result, n, i;
  char digits[6];
  ch = sys_net_connect(host);
  if (ch == SYS_ERR) return SYS_ERR;
  result = sl_wait_open(ch, WAIT, 1);
  if (result != SL_OPEN_OK) {
    if (result == SL_OPEN_CANCELLED) failure = "cancelled";
    return SYS_ERR;
  }
  request[0] = upload ? 'U' : 'D';
  request[1] = ' ';
  n = duration;
  i = 0;
  do {
    digits[i++] = (char)('0' + n % 10u);
    n /= 10u;
  } while (n);
  n = 2;
  while (i) request[n++] = digits[--i];
  request[n++] = '\n';
  request[n] = 0;
  if (send_text(ch, request) && read_line(ch) &&
      reply[0] == 'O' && reply[1] == 'K' && reply[2] == 0) return ch;
  sys_net_close(ch);
  return SYS_ERR;
}

static void rate(const char *label, uint32_t count) {
  sys_puts(label);
  sl_putu32(count * 60ul / duration);
  sys_puts(" B/s (");
  sl_putu32(count);
  sys_puts(" bytes)\n");
}

static void counters(void) {
  uint16_t frames = sys_net_stats();
  uint16_t resends = SNOS_ARGS.p0, sums = SNOS_ARGS.p1;
  sys_puts("frames ");
  sys_putu(frames);
  sys_puts(", bad sums ");
  sys_putu(sums);
  sys_puts(", resends ");
  sys_putu(resends);
  sys_putc('\n');
}

static int phase(uint16_t which) {
  uint16_t start, n, room, moved, i, empty = 0;
  int ok = 0;
  down = up = SYS_ERR;
  received = sent = remote = 0;
  if (which != 1) {
    down = prepare(0);
    if (down == SYS_ERR) goto done;
  }
  if (which != 0) {
    up = prepare(1);
    if (up == SYS_ERR) goto done;
  }
  if (up != SYS_ERR) {
    empty = sys_net_wspace(up);
    if (!send_text(up, "GO\n")) goto done;
  }
  if (down != SYS_ERR && !send_text(down, "GO\n")) goto done;
  /* No upload payload until the upload's GO is acknowledged (its ring has its
   * room back). The stack sends whatever a connection's ring holds, so payload
   * written straight after GO left in the same 1 KB segment, and reached the
   * peer -- which starts that direction's window on it -- frames after the
   * download's GO: the two windows of the duplex phase began apart. */
  if (up != SYS_ERR) {
    start = sys_frames();
    while (sys_net_wspace(up) < empty) {
      if (!alive(up) || expired(start, WAIT) || cancelled()) goto done;
      sys_yield();
    }
  }
  start = sys_frames();
  while (!expired(start, duration)) {
    moved = 0;
    if (down != SYS_ERR) {
      n = sys_net_read(down, incoming, BUF);
      if (n == SYS_ERR || !alive(down)) goto done;
      received += n;
      moved = n;
    }
    if (up != SYS_ERR) {
      room = sys_net_wspace(up);
      if (room != SYS_ERR && room > 0) {
        if (room > BUF) room = BUF;
        n = sys_net_write(up, data, room);
        if (n == SYS_ERR) goto done;
        sent += n;
        moved |= n;
      }
      if (!alive(up)) goto done;
    }
    if (cancelled()) goto done;
    if (!moved) sys_yield();
  }
  if (down != SYS_ERR) {
    sys_net_close(down);
    down = SYS_ERR;
  }
  if (up != SYS_ERR) {
    if (!read_line(up)) goto done;
    if (!reply[0]) goto bad_reply;
    for (i = 0; reply[i]; i++) {
      if (reply[i] < '0' || reply[i] > '9' || remote > 429496729ul ||
          (remote == 429496729ul && reply[i] > '5')) goto bad_reply;
      remote = remote * 10ul + (uint16_t)(reply[i] - '0');
    }
    if (remote > sent) goto bad_reply;
  }
  if (which == 0) rate("download: ", received);
  else if (which == 1) rate("upload: ", remote);
  else {
    rate("both download: ", received);
    rate("both upload: ", remote);
  }
  counters();
  ok = 1;
  goto done;
bad_reply:
  failure = "bad peer count";
done:
  if (down != SYS_ERR) sys_net_close(down);
  if (up != SYS_ERR) sys_net_close(up);
  return ok;
}

int main(void) {
  uint16_t i, seconds;
  char *p;
  failure = "peer failed or timed out";
  snos_getargs(args, sizeof args);
  p = args;
  while (*p && *p != ' ') p++;
  if (*p) *p++ = 0;
  while (*p == ' ') p++;
  seconds = 10;
  if (*p) {
    seconds = 0;
    while (*p >= '0' && *p <= '9') {
      if (seconds > 120u) goto usage;
      seconds = (uint16_t)(seconds * 10u + (uint16_t)(*p++ - '0'));
    }
    while (*p == ' ') p++;
    if (*p || seconds < 1 || seconds > 120) goto usage;
  }
  if (!args[0]) goto usage;
  sl_hostport(host, sizeof host, args, ":7778");
  duration = (uint16_t)(seconds * 60u);
  for (i = 0; i < BUF; i++) data[i] = 'x';
  sys_puts("speedtest ");
  sys_puts(host);
  sys_puts(" - ");
  sys_putu(seconds);
  sys_puts("s each way, then both; any key stops\n");
  counters();
  for (i = 0; i < 3; i++) {
    if (!phase(i)) {
      sys_puts("speedtest: ");
      sys_puts(failure);
      sys_putc('\n');
      return 1;
    }
  }
  return 0;
usage:
  sys_puts("net speedtest [host:port|-] [seconds 1..120]\n");
  return 1;
}
