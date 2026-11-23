/* LINKDIAG=1: the cable's counters, kept by the NMI and the link, painted by
 * the foreground on fixed screen rows about once a second (linkdiag.h,
 * docs/sa1-notes.md "The link diagnostic"). Built only with LINKDIAG.
 *
 * The NMI side stores raw values and keeps minima and maxima; nothing here
 * prints from the NMI. ld_nmi_entry (ldentry.s) runs before pad_latch, so it
 * only reads registers and stores them: pad_latch must still find the
 * auto-joypad read running (traps.c). */
#include "linkdiag.h"
#include "console.h"
#include "input.h"
#include "net.h"
#include "proc.h"
#include "snos.h"

#define R8(a) (*(volatile uint8_t *)(a))

#define LD_ROW 13  /* first screen row of the panel, under the boot text */
#define LD_EVERY 60u

uint8_t ld_net_ran;
uint16_t ld_guard;
/* ldentry.s fills these at NMI entry: $4212, V low, V high, H low, H high,
 * STAT78, and the in-NMI flag the link's counters split on. */
uint8_t ld_e[7];
#define in_nmi ld_e[6]

static uint16_t n_nmi, n_net, n_skip;
static uint16_t n_probe[2], n_clocked, n_refused, n_badlatch, n_ok, n_burnt;
static uint8_t ref_line, ref_ctx;
static uint8_t gate_min[2] = {255, 255}, gate_max[2];

static uint16_t ev_min = 0xFFFFu, ev_max, eh_min = 0xFFFFu, eh_max;
static uint16_t pv, pv_min = 0xFFFFu, pv_max;
static uint16_t g_min = 0xFFFFu, g_max;
static uint16_t len_min = 0xFFFFu, len_max;
static uint8_t s_last, stat_last;
static uint8_t hdr[6];
static uint16_t shown_at, refreshes;

/* The V counter, nine bits: $213F resets the read toggle, $2137 latches. */
static uint16_t v9(void) {
  uint16_t v;
  (void)R8(0x213F);
  (void)R8(0x2137);
  v = R8(0x213D);
  v |= (uint16_t)(R8(0x213D) & 1u) << 8;
  (void)R8(0x213F);
  return v;
}

static void minmax(uint16_t v, uint16_t *lo, uint16_t *hi) {
  if (v < *lo) *lo = v;
  if (v > *hi) *hi = v;
}

void ld_nmi_pad(void) { pv = v9(); }

void ld_nmi_exit(void) {
  uint16_t ev = (uint16_t)(ld_e[1] | ((uint16_t)(ld_e[2] & 1u) << 8));
  uint16_t eh = (uint16_t)(ld_e[3] | ((uint16_t)(ld_e[4] & 1u) << 8));
  uint16_t end = v9();
  n_nmi++;
  if (ld_net_ran) n_net++;
  else n_skip++;
  minmax(ev, &ev_min, &ev_max);
  minmax(eh, &eh_min, &eh_max);
  minmax(pv, &pv_min, &pv_max);
  minmax(ld_guard, &g_min, &g_max);
  if (ld_guard >= 2000u) n_burnt++;
  minmax(end >= ev ? end - ev : end + 262u - ev, &len_min, &len_max);
  s_last = ld_e[0];
  stat_last = ld_e[5]; /* STAT78 after the latch: bit 6 is the latch flag */
  in_nmi = 0;
}

void ld_probe(void) { n_probe[in_nmi]++; }

/* The vblank flag read beside the latched line: a line deep inside active
 * display with the flag set, or deep inside vblank without it, means the
 * latch did not happen. */
void ld_gate(uint8_t line) {
  uint8_t vb = (uint8_t)(R8(0x4212) & 0x80u);
  if (line < gate_min[in_nmi]) gate_min[in_nmi] = line;
  if (line > gate_max[in_nmi]) gate_max[in_nmi] = line;
  if ((vb && line >= 8u && line <= 220u) || (!vb && line >= 230u)) n_badlatch++;
}

void ld_refused(uint8_t line) {
  n_refused++;
  ref_line = line;
  ref_ctx = in_nmi;
}

void ld_clocked(void) { n_clocked++; }

void ld_rx_ok(const uint8_t *f) {
  int i;
  n_ok++;
  for (i = 0; i < 6; i++) hdr[i] = f[i];
}

/* ------------------------------------------------------------ the panel */

static char line[33];
static uint8_t at;

static void s(const char *p) {
  while (*p && at < 32) line[at++] = *p++;
}

static void c(char ch) {
  if (at < 32) line[at++] = ch;
}

static void u(uint16_t v) {
  char d[5];
  int n = 0;
  do {
    d[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n) c(d[--n]);
}

static void h2(uint8_t v) {
  static const char hex[] = "0123456789ABCDEF";
  c(hex[v >> 4]);
  c(hex[v & 15u]);
}

static void range(uint16_t lo, uint16_t hi) {
  if (lo > hi) {
    c('-');
    return;
  }
  u(lo);
  c('-');
  u(hi);
}

/* Only the cells that changed: an unchanged row stays clean, and the NMI
 * leaves the link alone only while rows are owed (traps.c). */
static void emit(int row) {
  int i;
  while (at < 32) line[at++] = ' ';
  for (i = 0; i < 32; i++)
    if (con_getat(LD_ROW + row, i) != (uint8_t)line[i]) con_putat(LD_ROW + row, i, line[i]);
  at = 0;
}

void ld_show(void) {
  uint16_t pw;
  int up;
  if (proc_current() != proc_fg()) return;
  if ((uint16_t)(snos_frames - shown_at) < LD_EVERY) return;
  shown_at = snos_frames;
  refreshes++;
  up = link_serial_diag(&pw);

#ifdef SA1
  s("LINKDIAG sa1 slowrom  t ");
#else
  s("LINKDIAG stock fastrom  t ");
#endif
  u(refreshes);
  emit(0);
  s("nmi ");
  u(n_nmi);
  s(" net ");
  u(n_net);
  s(" skip ");
  u(n_skip);
  emit(1);
  s("be ");
  c("-WS?"[link_backend() & 3]);
  s(" rdy");
  u((uint16_t)net_is_ready());
  s(" up");
  u((uint16_t)up);
  s(" pw");
  u(pw);
  s(" clk ");
  u(n_clocked);
  emit(2);
  s("prb n");
  u(n_probe[1]);
  s(" f");
  u(n_probe[0]);
  s(" ref ");
  u(n_refused);
  s(" @");
  u(ref_line);
  c(ref_ctx ? 'n' : 'f');
  emit(3);
  s("gate n");
  range(gate_min[1], gate_max[1]);
  s(" f");
  range(gate_min[0], gate_max[0]);
  s(" bl");
  u(n_badlatch);
  emit(4);
  s("in v");
  range(ev_min, ev_max);
  s(" h");
  range(eh_min, eh_max);
  s(" s");
  h2(s_last);
  s(" f");
  h2(stat_last);
  emit(5);
  s("pad");
  range(pv_min, pv_max);
  s(" g");
  range(g_min, g_max);
  s(" n");
  range(len_min, len_max);
  s(" b");
  u(n_burnt);
  emit(6);
  s("ok ");
  u(n_ok);
  s(" bad ");
  u(net_stat_bad_sum);
  s(" rx ");
  u(net_stat_rx_frames);
  emit(7);
  s("hdr ");
  for (pw = 0; pw < 6; pw++) {
    h2(hdr[pw]);
    c(' ');
  }
  emit(8);
}
