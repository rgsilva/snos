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

/* Kernel ABI version. Apps declare the minimum they need in their header;
 * the kernel refuses to exec an app that wants a newer ABI than it provides. */
#define SNOS_ABI_VERSION 1

/* ---- bank $00 RAM map (PLAN.md §2) — the 8 KB that caps everything ---- */
#define SNOS_KDP_BASE 0x0000u  /* kernel direct page          256 B */
#define SNOS_KSTACK_LO 0x0100u /* kernel stack               2816 B */
#define SNOS_KSTACK_HI 0x0BFFu
#define SNOS_ASTACK_LO 0x0C00u /* app stack                  3328 B */
#define SNOS_ASTACK_HI 0x18FFu
#define SNOS_ADP_BASE 0x1900u  /* app direct page             256 B */
#define SNOS_NEAR_BASE 0x1A00u /* kernel near data           1536 B */

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
uint16_t snos_cop_c(void);
void snos_brk_c(void);

extern volatile uint16_t snos_frames; /* incremented by the NMI handler */

#endif
