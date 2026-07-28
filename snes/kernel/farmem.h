/* Far memory helpers.
 *
 * The kernel constantly needs to reach memory in a bank it is not running in:
 * an app's ROM bank (to read its header, its strings, the COP signature), the
 * bank-$00 stack through its $7E mirror (to decode a trap frame), and cart
 * SRAM. All of it is 24-bit addressing, so it goes through here rather than
 * being open-coded with casts at every call site.
 */
#ifndef SNOS_FARMEM_H
#define SNOS_FARMEM_H

#include <stdint.h>

static uint8_t far_rd(uint16_t bank, uint16_t off) {
  return *(volatile __far uint8_t *)(((uint32_t)bank << 16) | (uint32_t)off);
}

static void far_wr(uint16_t bank, uint16_t off, uint8_t v) {
  *(volatile __far uint8_t *)(((uint32_t)bank << 16) | (uint32_t)off) = v;
}

static uint16_t far_rd16(uint16_t bank, uint16_t off) {
  return (uint16_t)(far_rd(bank, off) | ((uint16_t)far_rd(bank, (uint16_t)(off + 1)) << 8));
}

#endif
