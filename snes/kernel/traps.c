/* Trap handlers, entered from the asm shims in calypsi/header.s.
 *
 * All three live in bank $00 by way of those shims: COP, BRK and NMI force
 * PB=$00 before the vector fetch, so the *entry* must be in the bank-$00
 * mirror. These C bodies are reached by JSL and can live anywhere.
 *
 * M0 scope: NMI counts frames, COP and BRK are stubs. M2 fills in the real
 * syscall dispatch; M3 adds the kill switch to the NMI path.
 */
#include "snos.h"
#include "mailbox.h"

volatile uint16_t snos_frames;

/* Vblank, 60 Hz, kernel-owned unconditionally (PLAN.md §5). Runs on whatever
 * stack is current — while an app runs that is the APP's stack — so this must
 * stay shallow. No printf, no deep call chains, ever. */
void snos_nmi_c(void) { snos_frames++; }

/* Syscall dispatch. Stub until M2: returns 0 for everything. The signature
 * byte lives at [stacked return address - 1]; reading it is M2's job. */
uint16_t snos_cop_c(void) { return 0; }

/* Debugger trap. Never returns — parks the machine with a mailbox status the
 * Lua harness can distinguish from a hang. */
void snos_brk_c(void) {
  mb_puts("\n[BRK]\n");
  mb_finish(MB_STATUS_PANIC);
}
