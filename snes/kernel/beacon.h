/* The boot beacon (make SA1=1 BEACON=1, docs/sa1-notes.md): every
 * checkpoint before the console exists paints the whole screen one colour
 * and holds it for BEACON_HOLD frames, so the colour a TV is left showing
 * names the last checkpoint reached. After kcon_init the checkpoints are
 * words on one console line instead.
 *
 * Also included by calypsi/beacon.inc, so outside BEACON_ASM only #defines. */
#ifndef SNOS_BEACON_H
#define SNOS_BEACON_H

/* BGR555, in checkpoint order. */
#define BEACON_RED 0x001F     /* 1 reset vector, before the C startup */
#define BEACON_GREEN 0x03E0   /* 2 __low_level_init: first code from $C0+ */
#define BEACON_BLUE 0x7C00    /* 3 main() */
#define BEACON_YELLOW 0x03FF  /* 4 CCNT, SIE, SIC written */
#define BEACON_MAGENTA 0x7C1F /* 5 SBWE, SIWP written */
#define BEACON_CYAN 0x7FE0    /* 6 the I-RAM test */
#define BEACON_WHITE 0x7FFF   /* 7 a far call returned (BEACON_FASTROM: FastROM on) */
#define BEACON_ORANGE 0x019F  /* 8 mb_init, stack_paint */

#ifndef BEACON_HOLD
#define BEACON_HOLD 30 /* frames; the Makefile may set it */
#endif

#ifndef BEACON_ASM
#ifdef BEACON
#include <stdint.h>

#define BEACON_R8(a) (*(volatile uint8_t *)(a))

/* Busy-waits n frames on HVBJOY's vblank bit; needs no NMI. */
#define BEACON_WAIT(n)                                                 \
  do {                                                                 \
    uint8_t beacon_n;                                                  \
    for (beacon_n = (n); beacon_n; beacon_n--) {                       \
      while (BEACON_R8(0x4212) & 0x80) ;                               \
      while (!(BEACON_R8(0x4212) & 0x80)) ;                            \
    }                                                                  \
  } while (0)

/* Register writes and a $4212 poll only: no NMI, no library code. */
#define BEACON_AT(c)                                                   \
  do {                                                                 \
    BEACON_R8(0x2100) = 0x80;                                          \
    BEACON_R8(0x2121) = 0x00;                                          \
    BEACON_R8(0x2122) = (uint8_t)(c);                                  \
    BEACON_R8(0x2122) = (uint8_t)((c) >> 8);                           \
    BEACON_R8(0x2100) = 0x0F;                                          \
    BEACON_WAIT(BEACON_HOLD);                                          \
  } while (0)

void beacon_far(void); /* calypsi/beacon.inc, in another far bank */
#else
#define BEACON_AT(c)
#endif
#endif

#endif
