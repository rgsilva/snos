#include "sram.h"

#ifdef SA1
#define R8(a) (*(volatile uint8_t *)(a))

static uint8_t sa1_found;

/* Runs before NMI is enabled and before the first BW-RAM access, on every
 * boot and on `restart`. CCNT bit 5 holds the SA-1 in reset; SIE = 0 means it
 * can never raise an IRQ on this CPU, which has no handler for one. The Super
 * MMC values are its reset values: ROM megabytes 0-3 linear at $C0-$FF, ROM 0
 * at $00:8000. SBWE and SIWP open BW-RAM and I-RAM to this CPU: the sd2snes
 * and Mesen do not enforce them, the real chip does.
 *
 * Presence: I-RAM at $3000 keeps what is written there; without the chip
 * that address is open bus. The byte is put back. */
void sa1_hold(void) {
  uint8_t keep, ok;
  R8(0x2200) = 0x20; /* CCNT: SA-1 in reset, no IRQ or NMI to it */
  R8(0x2201) = 0x00; /* SIE: no IRQ from it */
  R8(0x2202) = 0xA0; /* SIC: clear its IRQ and character-conversion flags */
  R8(0x2220) = 0x00; /* CXB-FXB */
  R8(0x2221) = 0x01;
  R8(0x2222) = 0x02;
  R8(0x2223) = 0x03;
  R8(0x2226) = 0x80; /* SBWE */
  R8(0x2229) = 0xFF; /* SIWP: all eight 256-byte pages */
  keep = R8(0x3000);
  R8(0x3000) = 0x5A;
  ok = (uint8_t)(R8(0x3000) == 0x5A);
  R8(0x3000) = 0xA5;
  if (R8(0x3000) != 0xA5) ok = 0;
  R8(0x3000) = keep;
  sa1_found = ok;
}

uint8_t sa1_present(void) { return sa1_found; }
#endif


static uint32_t sram_bytes;

/* Turn a linear offset into a 24-bit address in ONE expression, and dereference
 * a far pointer directly.
 *
 * Not far_rd(bank, addr) with the two halves computed separately: that shape
 * miscompiled under Calypsi and silently returned a wrong byte at some offsets
 * (found via a single corrupted character in a ROM file — see
 * docs/calypsi-asm-syntax.md). Everything that touches storage now goes through
 * one direct __far pointer, and fstest verifies it across window boundaries. */
#ifdef SA1
static volatile __far uint8_t *sram_ptr(uint32_t off) {
  return (volatile __far uint8_t *)(SRAM_BASE + off);
}
#else
static volatile __far uint8_t *sram_ptr(uint32_t off) {
  uint32_t a = ((uint32_t)(SRAM_BANK0 + (uint16_t)(off >> SRAM_WINDOW_BITS)) << 16) |
               (uint32_t)(0x6000u + ((uint16_t)off & (uint16_t)(SRAM_WINDOW_SIZE - 1)));
  return (volatile __far uint8_t *)a;
}
#endif

uint8_t sram_rd(uint32_t off) { return *sram_ptr(off); }
void sram_wr(uint32_t off, uint8_t v) { *sram_ptr(off) = v; }

uint16_t sram_rd16(uint32_t off) {
  return (uint16_t)(sram_rd(off) | ((uint16_t)sram_rd(off + 1) << 8));
}
void sram_wr16(uint32_t off, uint16_t v) {
  sram_wr(off, (uint8_t)v);
  sram_wr(off + 1, (uint8_t)(v >> 8));
}

uint32_t sram_rd32(uint32_t off) {
  return (uint32_t)sram_rd16(off) | ((uint32_t)sram_rd16(off + 2) << 16);
}
void sram_wr32(uint32_t off, uint32_t v) {
  sram_wr16(off, (uint16_t)v);
  sram_wr16(off + 2, (uint16_t)(v >> 16));
}

/* Count the windows that are genuinely distinct storage rather than mirrors of
 * a smaller chip: write a per-window tag, then re-read every one. A window that
 * still holds its own tag is real; one that holds a later window's was aliased.
 *
 * This runs on EVERY boot, before the filesystem is mounted, and it writes to
 * offset 0 of each window — which is where the SRAMFS magic lives. So it has to
 * put back what it found, or it silently reformats the volume on every power-on
 * and nothing ever persists.
 *
 * The restore runs in DESCENDING order on purpose. With mirroring, several
 * logical windows are the same physical byte; going backwards means window 0's
 * saved value is written last and therefore wins, which is the one that matters
 * since it is the superblock.
 *
 * On SA-1 the windows are 8 KB steps through the first SA1_FS_KB of BW-RAM,
 * so the probe never writes the SA-1's work area above it. */
uint32_t sram_probe(void) {
  uint8_t saved[SRAM_MAX_WINDOWS];
  int16_t w;
  uint16_t distinct = 0;

  for (w = 0; w < SRAM_MAX_WINDOWS; w++)
    saved[w] = sram_rd((uint32_t)w << SRAM_WINDOW_BITS);
  for (w = 0; w < SRAM_MAX_WINDOWS; w++)
    sram_wr((uint32_t)w << SRAM_WINDOW_BITS, (uint8_t)(0x5A ^ w));
  for (w = 0; w < SRAM_MAX_WINDOWS; w++)
    if (sram_rd((uint32_t)w << SRAM_WINDOW_BITS) == (uint8_t)(0x5A ^ w)) distinct++;
  for (w = SRAM_MAX_WINDOWS - 1; w >= 0; w--)
    sram_wr((uint32_t)w << SRAM_WINDOW_BITS, saved[w]);

  sram_bytes = (uint32_t)distinct << SRAM_WINDOW_BITS;
  return sram_bytes;
}

uint32_t sram_size(void) { return sram_bytes; }
