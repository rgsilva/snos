/* Controller test, and the kill-switch proof.
 *
 * Loops forever showing the live pad bitmask. It exits cleanly on B, but it
 * deliberately has NO handling for START+SELECT: that chord is the kernel's
 * kill switch, and the point of this app is to demonstrate that the kernel can
 * take control back from a program that is not cooperating and never makes a
 * system call asking to be terminated.
 */
#include "snos_abi.h"

int main(void) {
  uint16_t last = 0xFFFFu;

  sys_puts("pad test: B exits, START+SELECT kills\n");
  sys_trace("PAD-RAN\n");

  for (;;) {
    uint16_t p = sys_pad();
    if (p != last) {
      sys_puts("pad=");
      sys_puthex(p);
      sys_putc('\n');
      last = p;
    }
    if (p & PAD_B) {
      sys_puts("pad: clean exit\n");
      return 7;
    }
    sys_yield(); /* hand the frame back: the OSK and console keep working */
  }
}
