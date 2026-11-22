/* Cartridge SRAM as one flat address space.
 *
 * HiROM maps battery SRAM as 8 KB windows at $6000-$7FFF in banks $20-$3F
 * (mirrored at $A0-$BF, which is what we use). That is 32 windows = 256 KB,
 * measured on this cart configuration by sram_probe() at boot rather than
 * assumed: the figure comes from snespdp11's sramprobe, and IDEAS.md records
 * that the older "Mesen caps SRAM at 64 KB" belief was a LoROM artifact.
 *
 * Callers see a linear 0..N space and never think about windows.
 */
#ifndef SNOS_SRAM_H
#define SNOS_SRAM_H

#include <stdint.h>

#define SRAM_WINDOW_BITS 13 /* 8 KB per window */
#define SRAM_WINDOW_SIZE (1ul << SRAM_WINDOW_BITS)
#ifdef SA1
/* SA-1 cartridge: BW-RAM, linear at $40:0000-$43:FFFF, one far pointer and no
 * windows (a "window" below is only the probe's 8 KB step). The filesystem
 * gets the first SA1_FS_KB; $43:0000 up is the SA-1's work area, which the
 * kernel never writes. The Makefile reads SA1_FS_KB for the tests. */
#define SRAM_BASE 0x400000ul
#define SA1_FS_KB 192u
#define SA1_WORK_BASE (SRAM_BASE + ((uint32_t)SA1_FS_KB << 10))
#define SA1_WORK_KB 64u
#define SRAM_MAX_WINDOWS (SA1_FS_KB / 8u)
#else
#define SRAM_BANK0 0xA0u    /* fast mirror of the $20-$3F SRAM banks */
#define SRAM_MAX_WINDOWS 32
#endif

#ifdef SA1
void sa1_hold(void);         /* first thing at boot: SA-1 in reset, BW-RAM open */
uint8_t sa1_present(void);   /* I-RAM answered at sa1_hold() */
#endif

uint32_t sram_probe(void); /* distinct bytes actually backed; called once */
uint32_t sram_size(void);

uint8_t sram_rd(uint32_t off);
void sram_wr(uint32_t off, uint8_t v);
uint16_t sram_rd16(uint32_t off);
void sram_wr16(uint32_t off, uint16_t v);
uint32_t sram_rd32(uint32_t off);
void sram_wr32(uint32_t off, uint32_t v);

#endif
