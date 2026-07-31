/* snoslib: the helpers every networked app had written for itself.
 *
 * The 2026-07-31 review measured ~450 duplicated lines across the apps; the
 * mechanical, single-behaviour part lives here now. Compiled INTO each app by
 * buildapp.sh (apps are separate links into their own banks — there is no
 * shared address space to put a library in), so the usual app rules apply,
 * the load-bearing one being: NO initialised writable file-scope data. Apps
 * link with --no-auto-libraries, so such initialisers are silently discarded
 * and read as zero — that is how BASIC's RND spent its whole life returning 0.
 *
 * Deliberately NOT here: get.c's open-wait (its timeout forensics — which
 * half of the link stalled, dot progress — are the richest version and are
 * the feature; flattening them into this simple waiter would be a loss), the
 * file-picker UIs, and the vnc/view PPU boilerplate (behavioural, each
 * subtly fitted to its app; see PLAN.md's backlog entry). */
#include "snoslib.h"

void sl_putu32(uint32_t v) {
  /* sys_putu takes 16 bits; transfer counters routinely exceed 64 KB. */
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

uint16_t sl_parse_port(const char *s, uint16_t def) {
  uint16_t v = 0;
  int any = 0;
  while (*s == ' ') s++;
  while (*s >= '0' && *s <= '9') {
    v = (uint16_t)(v * 10u + (uint16_t)(*s - '0'));
    s++;
    any = 1;
  }
  return any ? v : def;
}

/* Block until every byte is queued. The send queue drains at link speed, so
 * anything larger than it must be fed in as room appears; writing once and
 * hoping truncates the payload and the peer parses the fragment. EOF still
 * counts as writable: it means the PEER stopped sending, and the direction we
 * write in is still open — every HTTP client half-closes and then waits. */
int sl_send_all(uint16_t ch, const char *p, uint16_t n) {
  uint16_t sent = 0;
  while (sent < n) {
    uint16_t took = sys_net_write(ch, p + sent, (uint16_t)(n - sent));
    if (took == 0) {
      uint16_t st = sys_net_state(ch);
      if (st != NET_ST_OPEN && st != NET_ST_EOF) return 0;
      sys_yield();
      continue;
    }
    sent = (uint16_t)(sent + took);
  }
  return 1;
}

/* Wait out NET_ST_OPENING. Closes the channel on anything but success, so a
 * caller that gets a failure back has nothing to clean up. Cancellation by
 * keypress is offered because connects can take many seconds and a silent,
 * uninterruptible pause is indistinguishable from a hang. */
uint16_t sl_wait_open(uint16_t ch, uint16_t frames, int allow_cancel) {
  uint16_t deadline = (uint16_t)(sys_frames() + frames);
  uint16_t st;
  for (;;) {
    st = sys_net_state(ch);
    if (st != NET_ST_OPENING) break;
    if ((int16_t)(sys_frames() - deadline) >= 0) {
      sys_net_close(ch);
      return SL_OPEN_TIMEOUT;
    }
    if (allow_cancel && sys_pollkey() != SYS_ERR) {
      sys_net_close(ch);
      return SL_OPEN_CANCELLED;
    }
    sys_yield();
  }
  if (st != NET_ST_OPEN && st != NET_ST_EOF) {
    sys_net_close(ch);
    return SL_OPEN_FAILED;
  }
  return SL_OPEN_OK;
}

/* Copy src into dst, appending `suffix` (":6667" and the like) when src has
 * no colon of its own — nobody should have to type the default port. */
void sl_hostport(char *dst, int cap, const char *src, const char *suffix) {
  int i = 0, has = 0;
  while (src[i] && i < cap - 1) {
    if (src[i] == ':') has = 1;
    dst[i] = src[i];
    i++;
  }
  if (!has) {
    int j = 0;
    while (suffix[j] && i < cap - 1) dst[i++] = suffix[j++];
  }
  dst[i] = 0;
}
