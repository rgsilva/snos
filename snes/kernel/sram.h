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
#define SRAM_BANK0 0xA0u    /* fast mirror of the $20-$3F SRAM banks */
#define SRAM_MAX_WINDOWS 32

uint32_t sram_probe(void); /* distinct bytes actually backed; called once */
uint32_t sram_size(void);

uint8_t sram_rd(uint32_t off);
void sram_wr(uint32_t off, uint8_t v);
uint16_t sram_rd16(uint32_t off);
void sram_wr16(uint32_t off, uint16_t v);
uint32_t sram_rd32(uint32_t off);
void sram_wr32(uint32_t off, uint32_t v);

#endif
