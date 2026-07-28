/* The smallest possible SNOS application: prove exec and return work.
 *
 * Everything it does goes through COP system calls, and it shares not a single
 * symbol with the kernel — it is linked separately, into its own ROM bank. */
#include "snos_abi.h"

int main(void) {
  sys_puts("hello from a SNOS app\n");
  sys_puts("running in ROM bank ");
  sys_puthex(SNOS_ARGS.p2);
  sys_putc('\n');
  sys_trace("HELLO-RAN\n");
  return 42; /* the shell prints this, which proves the exit code survives */
}
