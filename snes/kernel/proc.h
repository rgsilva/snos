/* M10: processes. Step 1 built the context switch; step 5 makes it a
 * process model. See proc.c for the design. */
#ifndef SNOS_PROC_H
#define SNOS_PROC_H

#include <stdint.h>
#include "app.h"

#define PROC_MAX 3 /* 0 = the shell (+ its foreground app), 1..2 = spawned */

/* Zero the shim handshake words BEFORE NMI is enabled. They are fixed WRAM
 * addresses, not C statics, so they hold power-on garbage — and a garbage
 * PROC_WANT would make the very first vblank switch to a garbage stack
 * pointer. The exec flags in main.c had this exact bug; see the comment
 * there that cost an hour. */
void proc_init(void);

/* Called from snos_nmi_c every NMI: files the previous switch's saved stack
 * pointer, then decides whether the shim should switch on its way out. */
void proc_tick(void);

/* Which process the CPU is running right now, and which owns the console
 * and the keyboard. The two differ while a background process runs a slice. */
uint8_t proc_current(void);
uint8_t proc_fg(void);
void proc_set_fg(uint8_t id);

/* Spawn an application as a BACKGROUND process in bank-$00 slot 0. One at a
 * time for now (one spare slot). Returns the process id, or -1 with a reason
 * already printed. The caller must have set argv first and must wait out the
 * argv race (see shell.c). */
int proc_spawn_app(const app_entry_t *a);

/* SYS_EXIT from a non-zero process: mark it dead and rewrite its trap frame
 * so the COP shim returns into proc_grave instead of the app. sp is the
 * trap frame pointer snos_cop_c was given. */
void proc_exit_current(uint16_t sp, uint16_t code);

/* Mark a suspended process dead. Safe at any time: the scheduler's
 * mid-syscall deferral guarantees a suspended process is never inside a
 * syscall, so there is no kernel state to corrupt. */
int proc_kill(uint8_t id);

/* For `jobs`: 0 = free, 1 = live, 2 = dead-not-yet-reaped. */
uint8_t proc_state(uint8_t id);
const char *proc_name(uint8_t id);
uint16_t proc_exit_code(uint8_t id);
void proc_reap_slot(uint8_t id); /* free a dead PCB after reporting it */

/* Arena occupancy of live processes 1.., for the foreground launch check:
 * returns the id of a live process whose arena overlaps [page, page+pages),
 * or -1 if none. pages uses the header encoding (0 = 256). */
int proc_arena_conflict(uint8_t page, uint8_t pages);

/* The attention flag (SYS_NOTIFY): a background process asking for eyes.
 * Bits by process id; the console overlays an inverse '!' while any are set.
 * Cleared when the raiser is foregrounded, killed, or exits. */
void proc_notify(uint8_t id);
int proc_notify_pending(uint8_t id);

/* Offer the CPU from inside a blocking kernel loop (header.s). The switch
 * points are: NMI-outside-syscall, syscall exit, and THIS — together they
 * make it impossible for any process, foreground or background, blocked or
 * busy, to hold the CPU against the scheduler. */
void proc_switch_now(void);

/* Pin the current process across a multi-store kernel mutation that both the
 * shell (direct call) and a background syscall may run. Cheap, nestable; the
 * NMI and the link keep running, only context switches pause. */
void proc_lock(void);
void proc_unlock(void);

/* The step-1 instrument, kept: two kernel workers under the round-robin. */
void proc_mtest_start(uint16_t ticks);
int proc_busy(void);
uint32_t proc_test_a_count(void);
uint32_t proc_test_b_count(void);

#endif
