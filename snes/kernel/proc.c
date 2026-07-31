/* M10: processes on a stock SNES.
 *
 * Step 1 built the primitive on one observation, proven by the kill switch:
 * when NMI fires, the interrupted process's ENTIRE existence is its stack.
 * The CPU pushed PC and P; the shim pushes A/X/Y/D/DB on top
 * (calypsi/header.s). So a process control block is essentially one number —
 * the saved stack pointer — and a context switch is: store S, load S, rti.
 * This is app_exec's KSP trick (kernel/exec.s) made plural.
 *
 * Step 5 grew it into a process model. The load-bearing choices:
 *
 *   - Process 0 is the shell, AND its foreground application. app_exec is a
 *     synchronous call, so a foreground app simply IS process 0's
 *     continuation — the whole single-app machinery (KSP, kill switch,
 *     EXITFLAG) carries over untouched. Only background spawns are new.
 *   - A background process is an app entered through a FORGED trap frame
 *     (proc_forge): first entry and preemption-resume are the same rti.
 *     It runs muted — the trap layer refuses console, input and PPU
 *     syscalls unless the process holds the foreground (proc_fg).
 *   - Background SYS_EXIT cannot unwind (nothing to unwind to) and cannot
 *     park inside the COP handler (the mid-syscall deferral would pin the
 *     scheduler on it forever). Instead the handler marks the process DEAD
 *     and rewrites the trap frame's return address to proc_grave, a two-byte
 *     loop in bank $00; the next NMI switches away and never comes back.
 *   - proc_kill is safe at ANY time for a suspended process, and the
 *     deferral is why: a process is never suspended inside a syscall, so
 *     there is no half-finished kernel mutation to corrupt. The guarantee
 *     built for the kill switch pays for itself a third time.
 *
 * Division of labour with the shim, and why it is shaped this way:
 *
 *   - The DECISION runs here, in C, from snos_nmi_c: proc_tick() picks the
 *     next process and publishes its saved S at PROC_NEXT_S with PROC_WANT
 *     armed. C cannot perform the switch itself — the interrupted S exists
 *     only in the shim, under the pushed frame.
 *   - The SWAP runs in the shim tail (header.s), AFTER the kill-switch
 *     checks and behind the same mid-syscall deferral, so one syscall runs
 *     to completion before another process starts and the single $1B00
 *     syscall DP needs no locks. The shim stores the old S at PROC_OLD_S
 *     and sets PROC_SWITCHED.
 *   - The FILING runs here again, one NMI later: the first thing proc_tick
 *     does is move PROC_OLD_S into the previous process's PCB. Filing
 *     before deciding is what makes the one-frame lag safe — a slot's saved
 *     S is always filed before any decision could schedule that slot again.
 *
 * PROC_WANT is one-shot: the shim consumes it whether or not the deferral
 * allowed the switch, and a deferred switch is simply re-decided next NMI
 * from unchanged state.
 */
#include "proc.h"
#include "snos.h"
#include "fs.h"
#include "console.h"

/* Handshake words with the shim, in the kernel-private block next to the
 * exec state (exec.s owns $1A10-$1A16; these continue it). Fixed addresses
 * for the same reason KSP's is: the shim reaches them with `long:` stores. */
#define PROC_WANT (*(volatile __far uint16_t *)0x7E1A18ul)
#define PROC_NEXT_S (*(volatile __far uint16_t *)0x7E1A1Aul)
#define PROC_OLD_S (*(volatile __far uint16_t *)0x7E1A1Cul)
#define PROC_SWITCHED (*(volatile __far uint16_t *)0x7E1A1Eul)
/* Written by the SHIM at the instant of the swap (see header.s): identity
 * must change atomically with the stack, or the new process's first slice
 * runs mis-identified. proc_tick still files the old S a frame later; that
 * lag is harmless for bookkeeping and was a real bug for identity. */
#define PROC_NEXT_ID (*(volatile __far uint16_t *)0x7E1A20ul)
#define PROC_CUR_ID (*(volatile __far uint16_t *)0x7E1A22ul)

#define NETMASK_R8(a) (*(volatile uint8_t *)(a))
#define NETMASK_OFF() (NETMASK_R8(0x4200) = 0x01)
#define NETMASK_ON() (NETMASK_R8(0x4200) = 0x81)

#define ST_FREE 0
#define ST_LIVE 1
#define ST_DEAD 2

/* The dead-process parking loop, in bank $00 (header.s). */
extern void proc_grave(void);

typedef struct {
  uint16_t s;    /* saved stack pointer — the process, in one number */
  uint8_t state;
  uint8_t arena_page;
  uint16_t arena_pages; /* normalised: 1..256, 0 = no arena claim */
  uint16_t exit_code;
  char name[APP_NAME_LEN + 1];
} proc_t;

static proc_t procs[PROC_MAX];
static volatile uint8_t p_fg;    /* who owns the console and keyboard */
static uint8_t p_filing_prev;    /* whose S the shim saved at that switch */
static volatile uint8_t p_notify; /* attention bits, by process id */
/* Scheduler lock (M10 review). Kernel functions that both the shell (direct,
 * preemptible, kernel DP) and background syscalls (COP, deferral-atomic) may
 * run need mutual exclusion for their multi-store spans; pinning the current
 * process is the cheapest correct form. Locking consumes any pending switch
 * so cop_ret cannot take one mid-span; proc_tick files but arms nothing
 * while held. NMI itself still runs — net_service is unaffected. */
static volatile uint8_t p_lock;
static uint8_t sched_on;         /* mtest's bounded mode */
static uint16_t sched_ticks, sched_limit;

uint8_t proc_current(void) { return (uint8_t)PROC_CUR_ID; }

void proc_notify(uint8_t id) {
  if (id >= PROC_MAX) return;
  p_notify |= (uint8_t)(1u << id);
  con_notify(1);
}

int proc_notify_pending(uint8_t id) {
  return (p_notify >> id) & 1;
}

static void notify_clear(uint8_t id) {
  p_notify &= (uint8_t)~(1u << id);
  if (p_notify == 0) con_notify(0);
}
uint8_t proc_fg(void) { return p_fg; }
void proc_set_fg(uint8_t id) {
  p_fg = id;
  /* Foregrounding answers the notification by definition. */
  notify_clear(id);
}

uint8_t proc_state(uint8_t id) {
  return id < PROC_MAX ? procs[id].state : ST_FREE;
}
const char *proc_name(uint8_t id) {
  return id < PROC_MAX ? procs[id].name : "";
}
uint16_t proc_exit_code(uint8_t id) {
  return id < PROC_MAX ? procs[id].exit_code : 0;
}

void proc_reap_slot(uint8_t id) {
  if (id >= 1 && id < PROC_MAX && procs[id].state == ST_DEAD)
    procs[id].state = ST_FREE;
}

void proc_init(void) {
  /* Same hazard, same cure as the exec flags in main.c: these are fixed
   * WRAM words the NMI path reads every frame, and WRAM boots as garbage. */
  PROC_WANT = 0;
  PROC_SWITCHED = 0;
  PROC_NEXT_S = 0;
  PROC_OLD_S = 0;
  PROC_NEXT_ID = 0;
  PROC_CUR_ID = 0;
  procs[0].state = ST_LIVE; /* the shell exists by construction */
  p_fg = 0;
}

/* Forge the stack frame the shim's pop path expects, so a fresh process is
 * entered by the SAME rti that resumes a preempted one — one resume path,
 * zero special cases. Layout must match the shim's documented frame:
 * 1,s DB / 2,s D / 4,s Y / 6,s X / 8,s A / 10,s P / 11,s PC / 13,s PB.
 * Bank $00 RAM is written through its $7E mirror, same as the stack painter. */
static void wr0(uint16_t a, uint8_t v) {
  *(volatile __far uint8_t *)(SNOS_BANK0_MIRROR + a) = v;
}

static void proc_forge(int slot, uint8_t pb, uint16_t pc, uint16_t stack_top,
                       uint16_t dp, uint8_t db) {
  uint16_t t = stack_top;
  wr0(t, pb);
  wr0((uint16_t)(t - 1), (uint8_t)(pc >> 8));
  wr0((uint16_t)(t - 2), (uint8_t)pc);
  wr0((uint16_t)(t - 3), 0x00); /* P: 16-bit A/X, interrupts on */
  wr0((uint16_t)(t - 4), 0);    /* A hi */
  wr0((uint16_t)(t - 5), 0);    /* A lo */
  wr0((uint16_t)(t - 6), 0);    /* X hi */
  wr0((uint16_t)(t - 7), 0);    /* X lo */
  wr0((uint16_t)(t - 8), 0);    /* Y hi */
  wr0((uint16_t)(t - 9), 0);    /* Y lo */
  wr0((uint16_t)(t - 10), (uint8_t)(dp >> 8));
  wr0((uint16_t)(t - 11), (uint8_t)dp);
  wr0((uint16_t)(t - 12), db);
  procs[slot].s = (uint16_t)(t - 13);
}

int proc_arena_conflict(uint8_t page, uint8_t pages) {
  uint16_t lo = page, n = pages ? pages : 256u, i;
  for (i = 1; i < PROC_MAX; i++) {
    if (procs[i].state != ST_LIVE || procs[i].arena_pages == 0) continue;
    {
      uint16_t plo = procs[i].arena_page;
      uint16_t phi = (uint16_t)(plo + procs[i].arena_pages);
      if (lo < phi && (uint16_t)(lo + n) > plo) return (int)i;
    }
  }
  return -1;
}

int proc_spawn_app(const app_entry_t *a) {
  int i;
  /* One background process for now: bank-$00 slot 0 is the one spare, and
   * PCB 1 is wired to it. PCB 2 stays reserved for the mtest workers. */
  if (procs[1].state != ST_FREE) return -1;
  proc_forge(1, (uint8_t)a->bank, a->entry, (uint16_t)SNOS_PSTACK_TOP(0),
             (uint16_t)SNOS_PDP(0), 0x7F /* DB = arena, like exec.s */);
  procs[1].arena_page = a->arena_page;
  procs[1].arena_pages = a->arena_pages ? a->arena_pages : 256u;
  procs[1].exit_code = 0;
  for (i = 0; i <= APP_NAME_LEN; i++) procs[1].name[i] = a->name[i];
  procs[1].state = ST_LIVE; /* last: the scheduler may run it immediately */
  return 1;
}

void proc_exit_current(uint16_t sp, uint16_t code) {
  uint8_t id = (uint8_t)PROC_CUR_ID;
  if (id == 0 || id >= PROC_MAX) return; /* process 0 exits via app_return */
  procs[id].exit_code = code;
  procs[id].state = ST_DEAD;
  notify_clear(id); /* a dead process has nothing left to say */
  fs_reap(id); /* an abandoned write handle would lock out every writer */
  if (p_fg == id) p_fg = 0; /* a dead process cannot hold the console */
  /* Rewrite the trap frame so the COP shim's rti lands in the grave loop
   * rather than back in an app that believes it has exited. Offsets are the
   * shim frame's, reached through the bank-$00 mirror like everything else. */
  {
    uint32_t pcaddr = (uint32_t)proc_grave;
    wr0((uint16_t)(sp + 11), (uint8_t)pcaddr);
    wr0((uint16_t)(sp + 12), (uint8_t)(pcaddr >> 8));
    wr0((uint16_t)(sp + 13), 0x00); /* PB: proc_grave lives in bank $00 */
  }
}

int proc_kill(uint8_t id) {
  if (id < 1 || id >= PROC_MAX || procs[id].state != ST_LIVE) return -1;
  /* Safe even though the victim is mid-something: the deferral means a
   * suspended process is never inside a syscall, so its only state is its
   * own stack and arena, both of which die with it. */
  procs[id].exit_code = SNOS_EXIT_KILLED;
  procs[id].state = ST_DEAD;
  notify_clear(id);
  fs_reap(id);
  if (p_fg == id) p_fg = 0;
  return 0;
}

/* ------------------------------------------------------------- scheduling */

static int runnable(uint8_t i) { return procs[i].state == ST_LIVE; }

void proc_lock(void) {
  NETMASK_OFF();
  PROC_WANT = 0;
  p_lock++;
  NETMASK_ON();
}

void proc_unlock(void) {
  if (p_lock) p_lock--;
}

void proc_tick(void) {
  /* File first, decide second — see the header comment for why the order is
   * load-bearing. */
  uint8_t cur = (uint8_t)PROC_CUR_ID;
  if (PROC_SWITCHED) {
    PROC_SWITCHED = 0;
    procs[p_filing_prev].s = PROC_OLD_S;
  }

  /* The foreground app's death is armed. Only process 0 can take the unwind
   * (the shims defer it from any other context), so steer the rotation to
   * process 0 and arm nothing else until the funeral is done. */
  if (SNOS_EXEC_RUN && SNOS_EXEC_FLAG) {
    if (cur != 0 && !p_lock) {
      p_filing_prev = cur;
      PROC_NEXT_S = procs[0].s;
      PROC_NEXT_ID = 0;
      PROC_WANT = 1;
    }
    return;
  }

  /* mtest's bounded mode: wind down to process 0, then stop. */
  if (sched_on && ++sched_ticks >= sched_limit) {
    if (cur == 0) {
      sched_on = 0;
      PROC_WANT = 0; /* a deferred switch must not resurrect a freed worker */
      procs[1].state = ST_FREE;
      procs[2].state = ST_FREE;
      return;
    }
    p_filing_prev = cur;
    PROC_NEXT_S = procs[0].s;
    PROC_NEXT_ID = 0;
    PROC_WANT = 1;
    return;
  }

  if (p_lock) return; /* pinned: file (above) but arm nothing */

  /* Round robin over live processes. With only process 0 live this decides
   * "stay" in two array reads — the everyday cost of having a scheduler. */
  {
    uint8_t n = cur;
    do {
      n = (uint8_t)((n + 1) % PROC_MAX);
    } while (!runnable(n) && n != cur);
    if (n == cur || !runnable(n)) return;
    p_filing_prev = cur;
    PROC_NEXT_S = procs[n].s;
    PROC_NEXT_ID = n;
    PROC_WANT = 1;
  }
}

/* --------------------------------------------------- the step-1 instrument */

static volatile uint32_t test_a, test_b;

uint32_t proc_test_a_count(void) { return test_a; }
uint32_t proc_test_b_count(void) { return test_b; }

static void worker_a(void) {
  for (;;) test_a++;
}
static void worker_b(void) {
  for (;;) test_b++;
}

void proc_mtest_start(uint16_t ticks) {
  uint32_t fa = (uint32_t)worker_a, fb = (uint32_t)worker_b;
  test_a = 0;
  test_b = 0;
  /* The process slots are idle (the caller refuses to run over an app), so
   * the workers take one each — the same carving real processes get. */
  proc_forge(1, (uint8_t)(fa >> 16), (uint16_t)fa,
             (uint16_t)SNOS_PSTACK_TOP(1), (uint16_t)SNOS_PDP(1), 0x00);
  proc_forge(2, (uint8_t)(fb >> 16), (uint16_t)fb,
             (uint16_t)SNOS_PSTACK_TOP(0), (uint16_t)SNOS_PDP(0), 0x00);
  procs[1].arena_pages = 0; /* kernel workers claim no arena */
  procs[2].arena_pages = 0;
  procs[1].state = ST_LIVE;
  procs[2].state = ST_LIVE;
  sched_ticks = 0;
  sched_limit = ticks;
  sched_on = 1;
}

int proc_busy(void) { return sched_on; }
