/* M10 step 1: the context-switch primitive, and nothing else.
 *
 * The whole design rests on one observation, proven by the kill switch: when
 * NMI fires, the interrupted process's ENTIRE existence is its stack. The CPU
 * pushed PC and P; the shim pushes A/X/Y/D/DB on top (calypsi/header.s). So a
 * process control block is essentially one number — the saved stack pointer —
 * and a context switch is: store S, load S, rti. This is app_exec's KSP trick
 * (kernel/exec.s) made plural.
 *
 * Division of labour with the shim, and why it is shaped this way:
 *
 *   - The DECISION runs here, in C, from snos_nmi_c: proc_tick() picks the
 *     next process and publishes its saved S at PROC_NEXT_S with PROC_WANT
 *     armed. C cannot perform the switch itself — the interrupted S exists
 *     only in the shim, under the pushed frame.
 *   - The SWAP runs in the shim tail (header.s), AFTER the kill-switch
 *     checks and behind the same mid-syscall deferral: a process inside a
 *     COP handler is never switched away from, so one syscall runs to
 *     completion before another can start and the kernel is never
 *     re-entered. The single $1B00 syscall DP stays correct with no locks.
 *     The shim stores the old S at PROC_OLD_S and sets PROC_SWITCHED.
 *   - The FILING runs here again, one NMI later: the first thing proc_tick
 *     does is move PROC_OLD_S into the previous process's PCB. Filing
 *     before deciding is what makes the one-frame lag safe — a slot's saved
 *     S is always filed before any decision could schedule that slot again.
 *
 * PROC_WANT is one-shot: the shim consumes it whether or not the deferral
 * allowed the switch, and a deferred switch is simply re-decided next NMI
 * from unchanged state.
 *
 * Step 1 deliberately stops here: two kernel-resident workers with hardwired
 * stack/DP carvings borrowed from the (guaranteed idle) app region, driven by
 * `debug mtest`. Real spawn/slot/UX machinery is steps 3-5; blocking-syscall
 * yielding is step 2. The workers' only job is to be unschedulable by
 * anything except genuine preemption: they never yield, never trap, never
 * return — an infinite counter bump each. Both counters advancing is a fact
 * only a working preemptive switch can produce.
 */
#include "proc.h"
#include "snos.h"

/* Handshake words with the shim, in the kernel-private block next to the
 * exec state (exec.s owns $1A10-$1A16; these continue it). Fixed addresses
 * for the same reason KSP's is: the shim reaches them with `long:` stores. */
#define PROC_WANT (*(volatile __far uint16_t *)0x7E1A18ul)
#define PROC_NEXT_S (*(volatile __far uint16_t *)0x7E1A1Aul)
#define PROC_OLD_S (*(volatile __far uint16_t *)0x7E1A1Cul)
#define PROC_SWITCHED (*(volatile __far uint16_t *)0x7E1A1Eul)

#define PROC_MAX 3 /* step 1: the shell + two workers */

typedef struct {
  uint16_t s;   /* saved stack pointer — the process, in one number */
  uint8_t live;
} proc_t;

static proc_t procs[PROC_MAX];
static uint8_t p_cur;          /* who the CPU is running right now */
static uint8_t p_pending_next; /* chosen by the last decision */
static uint8_t p_filing_prev;  /* whose S the shim saved at that switch */
static uint8_t sched_on;
static uint16_t sched_ticks, sched_limit;

/* Worker progress counters. 32-bit: a tight bump loop wraps 16 bits well
 * within one timeslice, and a wrapped counter could alias to zero exactly
 * when the verdict reads it. */
static volatile uint32_t test_a, test_b;

uint32_t proc_test_a_count(void) { return test_a; }
uint32_t proc_test_b_count(void) { return test_b; }

void proc_init(void) {
  /* Same hazard, same cure as the exec flags in main.c: these are fixed
   * WRAM words the NMI path reads every frame, and WRAM boots as garbage. */
  PROC_WANT = 0;
  PROC_SWITCHED = 0;
  PROC_NEXT_S = 0;
  PROC_OLD_S = 0;
}

/* Never called, never returns, never yields: pure preemption fodder. */
static void worker_a(void) {
  for (;;) test_a++;
}
static void worker_b(void) {
  for (;;) test_b++;
}

/* Forge the stack frame the shim's pop path expects, so a fresh process is
 * entered by the SAME rti that resumes a preempted one — one resume path,
 * zero special cases. Layout must match the shim's documented frame:
 * 1,s DB / 2,s D / 4,s Y / 6,s X / 8,s A / 10,s P / 11,s PC / 13,s PB.
 * Bank $00 RAM is written through its $7E mirror, same as the stack painter. */
static void wr0(uint16_t a, uint8_t v) {
  *(volatile __far uint8_t *)(SNOS_BANK0_MIRROR + a) = v;
}

static void proc_forge(int slot, void (*fn)(void), uint16_t stack_top,
                       uint16_t dp) {
  uint32_t fa = (uint32_t)fn;
  uint16_t pc = (uint16_t)fa;
  uint16_t t = stack_top;
  wr0(t, (uint8_t)(fa >> 16));            /* PB */
  wr0((uint16_t)(t - 1), (uint8_t)(pc >> 8));
  wr0((uint16_t)(t - 2), (uint8_t)pc);    /* PC: the worker's entry */
  wr0((uint16_t)(t - 3), 0x00);           /* P: 16-bit A/X, interrupts on */
  wr0((uint16_t)(t - 4), 0);              /* A hi */
  wr0((uint16_t)(t - 5), 0);              /* A lo */
  wr0((uint16_t)(t - 6), 0);              /* X hi */
  wr0((uint16_t)(t - 7), 0);              /* X lo */
  wr0((uint16_t)(t - 8), 0);              /* Y hi */
  wr0((uint16_t)(t - 9), 0);              /* Y lo */
  wr0((uint16_t)(t - 10), (uint8_t)(dp >> 8));
  wr0((uint16_t)(t - 11), (uint8_t)dp);   /* D: the slot's direct page */
  wr0((uint16_t)(t - 12), 0x00);          /* DB: kernel near data, bank $00 */
  procs[slot].s = (uint16_t)(t - 13);
  procs[slot].live = 1;
}

void proc_mtest_start(uint16_t ticks) {
  test_a = 0;
  test_b = 0;
  /* The process slots are idle (the caller refuses to run over an app), so
   * the workers take one each — the same carving real processes get (step 3,
   * SNOS_PSTACK_* in snos.h). */
  proc_forge(1, worker_a, SNOS_PSTACK_TOP(1), SNOS_PDP(1));
  proc_forge(2, worker_b, SNOS_PSTACK_TOP(0), SNOS_PDP(0));
  procs[0].live = 1; /* the caller; its S is filed by the first switch away */
  p_cur = 0;
  sched_ticks = 0;
  sched_limit = ticks;
  PROC_SWITCHED = 0;
  PROC_WANT = 0;
  sched_on = 1;
}

int proc_busy(void) { return sched_on; }

void proc_tick(void) {
  /* File first, decide second — see the header comment for why the order is
   * load-bearing. */
  if (PROC_SWITCHED) {
    PROC_SWITCHED = 0;
    procs[p_filing_prev].s = PROC_OLD_S;
    p_cur = p_pending_next;
  }
  if (!sched_on) return;

  if (++sched_ticks >= sched_limit) {
    /* Wind down: force control back to process 0, then stop scheduling. The
     * workers are simply never run again; their borrowed region reverts to
     * being the app region. */
    if (p_cur == 0) {
      sched_on = 0;
      procs[1].live = 0;
      procs[2].live = 0;
      return;
    }
    p_pending_next = 0;
  } else {
    uint8_t n = p_cur;
    do {
      n = (uint8_t)((n + 1) % PROC_MAX);
    } while (!procs[n].live);
    if (n == p_cur) return;
    p_pending_next = n;
  }
  p_filing_prev = p_cur;
  PROC_NEXT_S = procs[p_pending_next].s;
  PROC_WANT = 1;
}
