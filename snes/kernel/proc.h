/* M10 step 1: the context-switch primitive. See proc.c. */
#ifndef SNOS_PROC_H
#define SNOS_PROC_H

#include <stdint.h>

/* Zero the shim handshake words BEFORE NMI is enabled. They are fixed WRAM
 * addresses, not C statics, so they hold power-on garbage — and a garbage
 * PROC_WANT would make the very first vblank switch to a garbage stack
 * pointer. The exec flags in main.c had this exact bug; see the comment
 * there that cost an hour. */
void proc_init(void);

/* Called from snos_nmi_c every NMI: files the previous switch's saved stack
 * pointer, then decides whether the shim should switch on its way out. */
void proc_tick(void);

/* Spawn the two kernel-resident test workers and run the round-robin for
 * `ticks` NMIs, ending with control forced back to process 0 (the caller). */
void proc_mtest_start(uint16_t ticks);
int proc_busy(void);
uint32_t proc_test_a_count(void);
uint32_t proc_test_b_count(void);

#endif
