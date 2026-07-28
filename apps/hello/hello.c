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
  /* Launch arguments, ABI 4. Also the smallest possible proof that the shell
   * hands them over and that they are cleared between launches. */
  {
    char args[64];
    if (snos_getargs(args, sizeof args) > 0) {
      sys_puts("args: [");
      sys_puts(args);
      sys_puts("]\n");
    } else {
      sys_puts("args: (none)\n");
    }
  }
  sys_trace("HELLO-RAN\n");
  return 42; /* the shell prints this, which proves the exit code survives */
}
