/* SNOS kernel-wide definitions.
 *
 * The bank-$00 map here MUST agree with calypsi/linker.scm. That file reserves
 * the app region by omission; these constants are what the kernel uses to hand
 * those addresses to an app at exec time (M3).
 */
#ifndef SNOS_H
#define SNOS_H

#include <stdint.h>

#define SNOS_VERSION_MAJOR 0
#define SNOS_VERSION_MINOR 1
#define SNOS_VERSION_STR "0.1"

/* The ABI version lives in snos_abi.h, which is the header applications share.
 * It used to be defined here as well, and two definitions of the one number
 * that says what the kernel can do is exactly as good an idea as it sounds:
 * whichever header was included last won, so the kernel could report an ABI it
 * did not have and refuse to run an application over it. */

/* ---- bank $00 RAM map (PLAN.md §2) — the 8 KB that caps everything ---- */
#define SNOS_KDP_BASE 0x0000u  /* kernel direct page          256 B */
#define SNOS_KSTACK_LO 0x0100u /* kernel stack               2816 B */
#define SNOS_KSTACK_HI 0x0BFFu

/* The old single-app region ($0C00-$19FF, stack + DP) carved into two
 * process slots (M10 step 3). Slot sizes are MEASURED, not guessed: painted
 * app stacks across the whole suite peak at 1,123 bytes (editor), with basic
 * at its expression-depth cap using 1,051 — so 1,536 per slot carries 400+
 * bytes of margin over the worst case ever observed. Slot 1 keeps the
 * historical DP page ($1900), so the single-app path changes only its stack
 * ceiling. Calypsi addresses stack and DP through S and D, which are
 * per-process registers — no app relinking, only these assignments. */
#define SNOS_PSLOTS 2u
#define SNOS_PSTACK_SIZE 1536u
#define SNOS_PSTACK_LO(i) (0x0C00u + (i) * SNOS_PSTACK_SIZE)
#define SNOS_PSTACK_TOP(i) (SNOS_PSTACK_LO(i) + SNOS_PSTACK_SIZE - 1u)
#define SNOS_PDP(i) (0x1800u + (i) * 256u)

/* Compatibility names: the single-app path runs in slot 1. */
#define SNOS_ASTACK_LO SNOS_PSTACK_LO(1)  /* $1200 */
#define SNOS_ASTACK_HI SNOS_PSTACK_TOP(1) /* $17FF */
#define SNOS_ADP_BASE SNOS_PDP(1)         /* $1900 */
/* $1A00-$1FFF: syscall parameter block ($1A00), exec+scheduler state
 * ($1A10-$1A23), trap DPs ($1B00/$1C00), kernel near data ($1D00-$1FFF).
 * (The old SNOS_NEAR_BASE macro mislabelled this and was referenced
 * nowhere.) */

/* Bank $00 is mirrored from WRAM $7E0000, so the kernel can reach the same
 * bytes as far pointers without needing DB games. Used by the stack painter. */
#define SNOS_BANK0_MIRROR 0x7E0000ul

/* ---- app arena (WRAM bank $7F) ---- */
#define SNOS_ARENA_BASE 0x7F0000ul
#define SNOS_ARENA_SIZE 0x10000ul

/* ---- ROM layout ---- */
#define SNOS_APP_BANK_FIRST 0xC8u /* apps live in HiROM banks $C8..$FF */
#define SNOS_APP_BANK_LAST 0xFFu

/* ---- console helpers (kernel side, wrapping console.c) ---- */
void kcon_init(void);
void kcon_puts(const char *s);
void kcon_putc(int c);
void kcon_putu(uint16_t v);      /* unsigned decimal */
void kcon_putu32(uint32_t v);    /* unsigned decimal, 32-bit */
void kcon_puthex16(uint16_t v);  /* 4 hex digits */
void kcon_sync(void);            /* flush every dirty row, not just one */

/* ---- stack instrumentation (stack.s) ----
 * The 8 KB bank-$00 budget is the design's tightest constraint, so measure it
 * instead of hoping. stack_paint() fills the unused part of the kernel stack
 * with a pattern at boot; stack_low() reports the lowest address ever touched.
 * The Lua harness asserts this stays inside its slot. */
void stack_paint(void);
uint16_t stack_low(void);

/* ---- traps (traps.c, entered from calypsi/header.s) ---- */
void snos_nmi_c(void);
uint16_t snos_cop_c(uint16_t sp); /* sp = trap frame pointer; see traps.c */
void snos_brk_c(uint16_t sp);

/* shell.c */
void shell_run(void);
void snos_restart(void); /* exec.s; shell only, jobs stopped; never returns */

/* ---- exec state (kernel/exec.s) ----
 * Fixed addresses inside the block linker.scm reserves at $1A00-$1A3F.
 * $1A00-$1A0F is the syscall parameter block (ABI); from $1A10 up is
 * kernel-private. Reached through the $7E mirror so they work from any bank. */
#define SNOS_EXEC_KSP (*(volatile __far uint16_t *)0x7E1A10ul)
#define SNOS_EXEC_FLAG (*(volatile __far uint16_t *)0x7E1A12ul)
#define SNOS_EXEC_CODE (*(volatile __far uint16_t *)0x7E1A14ul)
#define SNOS_EXEC_RUN (*(volatile __far uint16_t *)0x7E1A16ul)

/* Exit code reported when the kill switch terminates an app. */
#define SNOS_EXIT_KILLED 0xFFFFu
/* How long START+SELECT must be held. Long enough not to fire on a chord an
 * app uses deliberately, short enough to feel like an escape hatch. */
#define SNOS_KILL_FRAMES 45

uint16_t app_exec(void); /* exec.s; args in SNOS_ARGS.p2/p3 */

extern volatile uint16_t snos_frames; /* incremented by the NMI handler */

/* Set by the NMI handler when the host presses the break key (F12) and no
 * application is running. Long-running shell commands should poll and clear it
 * so they can be interrupted; the kill switch covers applications already. */
extern volatile uint16_t snos_break;

static int snos_break_taken(void) {
  if (!snos_break) return 0;
  snos_break = 0;
  return 1;
}

#endif
