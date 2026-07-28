/* SNOS kernel entry.
 *
 * M0 scope: come up, prove the toolchain and the memory map, print a banner,
 * report the stack watermark, park. The shell arrives in M2.
 */
#include "snos.h"
#include "console.h"
#include "mailbox.h"
#include "input.h"
#include "app.h"
#include "snos_abi.h"
#include "sram.h"
#include "fs.h"

#define R8(a) (*(volatile uint8_t *)(a))

static void hw_init(void) {
  /* FastROM. Reset always begins slow with PB=$00 (the $00:8000 mirror), so
   * this is the first thing worth doing — the linker puts our code in $C0+,
   * which is exactly the window MEMSEL accelerates. Measured ~14% in snesmac
   * (docs/PERF.md item C). */
  R8(0x420D) = 0x01;

  /* NMI is enabled by kbd_init() once the handler has something to do. The
   * kernel owns it unconditionally from here on (PLAN.md §5). */
  R8(0x4200) = 0x00;

  /* Clear the exec state BEFORE anything can enable NMI.
   *
   * These flags live in WRAM, and WRAM contains garbage at power-on. The NMI
   * handler checks them on every single frame to decide whether to unwind a
   * running application — so if they come up non-zero, the very first vblank
   * jumps to app_return with a junk saved stack pointer and destroys the stack
   * before an application has ever existed. Cost an hour; the BRK register
   * dump (Y = the stack-paint pattern, S near the bottom of its slot) is what
   * gave it away. */
  SNOS_EXEC_RUN = 0;
  SNOS_EXEC_FLAG = 0;
  SNOS_EXEC_CODE = 0;
  SNOS_EXEC_KSP = 0;
  SNOS_ARGS.p0 = 0;
  SNOS_ARGS.p1 = 0;
  SNOS_ARGS.p2 = 0;
  SNOS_ARGS.p3 = 0;
}

static void banner(void) { kcon_puts("v" SNOS_VERSION_STR "\n"); }

/* The bank-$00 budget is the design's tightest constraint (PLAN.md §2), so the
 * kernel stack watermark is reported on every boot — but to the test mailbox,
 * not to the screen. The harness asserts on it; a user has no use for it, and
 * `mem` shows it on demand. */
static void report_stack(void) {
  mb_puts("STACKLOW ");
  mb_puthex32(stack_low());
  mb_putc('\n');
}

int main(void) {
  hw_init();
  mb_init();
  stack_paint();

  kcon_init();
  banner();
  report_stack();
  kbd_init();
  app_scan(); /* walk the ROM banks looking for application headers */

  {
    uint32_t n = sram_probe();
    mb_puts("SRAMKB ");
    mb_puthex32(n >> 10);
    mb_putc('\n');
  }
  fs_init();

  /* One line of genuinely useful state: how much storage there is, and what is
   * on it. The SRAM figure is measured at this boot, not compiled in. */
  kcon_putu((uint16_t)(sram_size() >> 10));
  kcon_puts("K SRAM\n");
  kcon_puts("SELECT opens the on-screen keyboard\n");

  shell_run(); /* never returns */
  return 0;
}
