/* Trap handlers, entered from the asm shims in calypsi/header.s.
 *
 * COP, BRK and NMI all force PB=$00 before the vector fetch, so their *entry
 * points* must live in the bank-$00 mirror. These C bodies are reached by JSL
 * and can live anywhere.
 *
 * ------------------------------------------------------------- the frame
 *
 * Both trap shims push the same thing in the same order, so one set of offsets
 * describes both. They hand this function the stack pointer as it stood after
 * those pushes; everything is read relative to that. The stack lives in bank
 * $00, which is the WRAM mirror of $7E0000, so the frame is reachable with
 * ordinary long addressing and no DB games.
 *
 *   sp+1  DB      sp+2  D      sp+4  Y      sp+6  X      sp+8  A
 *   sp+10 P       sp+11 PC     sp+13 PB
 *
 * The 65816 pushes PC already advanced past the COP signature byte, so the
 * signature is the byte at PB:PC-1. That is what makes `COP #n` a one-
 * instruction system call: the number is baked into the instruction stream and
 * the handler reads it back out.
 */
#include "snos.h"
#include "snos_abi.h"
#include "console.h"
#include "input.h"
#include "mailbox.h"

#define BANK0M 0x7E0000ul

#define FRM_DB 1
#define FRM_D 2
#define FRM_Y 4
#define FRM_X 6
#define FRM_A 8
#define FRM_P 10
#define FRM_PC 11
#define FRM_PB 13

volatile uint16_t snos_frames;

static uint8_t frm8(uint16_t sp, uint16_t off) {
  return *(volatile __far uint8_t *)(BANK0M + (uint32_t)(uint16_t)(sp + off));
}

static uint16_t frm16(uint16_t sp, uint16_t off) {
  return (uint16_t)(frm8(sp, off) | ((uint16_t)frm8(sp, off + 1) << 8));
}

/* Read a byte from an arbitrary bank. Used for the COP signature (which is in
 * the caller's code bank, i.e. an app's ROM bank) and for string arguments. */
static uint8_t far_rd(uint16_t bank, uint16_t off) {
  return *(volatile __far uint8_t *)(((uint32_t)bank << 16) | (uint32_t)off);
}

static void far_wr(uint16_t bank, uint16_t off, uint8_t v) {
  *(volatile __far uint8_t *)(((uint32_t)bank << 16) | (uint32_t)off) = v;
}

/* Vblank, 60 Hz, kernel-owned unconditionally (PLAN.md §5). Runs on whatever
 * stack is current — while an app runs that is the APP's stack — so this must
 * stay shallow. No printf, no deep call chains, ever. */
void snos_nmi_c(void) {
  snos_frames++;
  pad_latch();
}

/* ----------------------------------------------------------- COP dispatch */

static char rl_buf[128];

uint16_t snos_cop_c(uint16_t sp) {
  uint16_t pc = frm16(sp, FRM_PC);
  uint16_t pb = frm8(sp, FRM_PB);
  uint8_t sig = far_rd(pb, (uint16_t)(pc - 1));
  uint16_t a = frm16(sp, FRM_A);

  switch (sig) {
  case SYS_EXIT:
    /* M3 restores the kernel's saved stack pointer here and returns to the
     * shell. With no apps yet there is nothing to return to. */
    return 0;

  case SYS_VERSION:
    return SNOS_ABI_VERSION;

  case SYS_YIELD:
    kbd_idle();
    return 0;

  case SYS_PUTC:
    kcon_putc((int)(a & 0xFFu));
    return 0;

  case SYS_PUTS: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint8_t c;
    while ((c = far_rd(bank, off)) != 0) {
      kcon_putc(c);
      off++;
    }
    return 0;
  }

  case SYS_GETC:
    return (uint16_t)kbd_getchar();

  case SYS_CLS:
    con_clear();
    return 0;

  case SYS_READLINE: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint16_t max = a, i;
    int n;
    if (max > sizeof rl_buf) max = sizeof rl_buf;
    n = kbd_readline(rl_buf, (int)max);
    for (i = 0; i <= (uint16_t)n; i++) far_wr(bank, (uint16_t)(off + i), (uint8_t)rl_buf[i]);
    return (uint16_t)n;
  }

  case SYS_PUTU:
    kcon_putu(a);
    return 0;

  case SYS_PUTHEX:
    kcon_puthex16(a);
    return 0;

  case SYS_FRAMES:
    return snos_frames;

  case SYS_WAITVBL: {
    uint16_t f = snos_frames;
    while (snos_frames == f) {
    }
    return 0;
  }

  case SYS_TRACE: {
    uint16_t off = SNOS_ARGS.p0, bank = SNOS_ARGS.p1;
    uint8_t c;
    while ((c = far_rd(bank, off)) != 0) {
      mb_putc((char)c);
      off++;
    }
    return 0;
  }

  default:
    return SYS_ERR;
  }
}

/* --------------------------------------------------------------- BRK ---- */

static void dump(const char *name, uint16_t v) {
  kcon_puts(name);
  kcon_puthex16(v);
  kcon_putc(' ');
}

/* Debugger trap. Never returns. Prints a register dump on screen AND into the
 * mailbox, so a panic is legible both to a human watching the TV and to the
 * automated harness. */
void snos_brk_c(uint16_t sp) {
  kcon_puts("\n*** BRK ***\n");
  dump("PB:", frm8(sp, FRM_PB));
  dump("PC:", frm16(sp, FRM_PC));
  dump("P:", frm8(sp, FRM_P));
  kcon_putc('\n');
  dump("A:", frm16(sp, FRM_A));
  dump("X:", frm16(sp, FRM_X));
  dump("Y:", frm16(sp, FRM_Y));
  kcon_putc('\n');
  dump("D:", frm16(sp, FRM_D));
  dump("DB:", frm8(sp, FRM_DB));
  dump("S:", sp);
  kcon_putc('\n');
  kcon_sync();

  mb_puts("\n[BRK]\n");
  mb_finish(MB_STATUS_PANIC);
}
