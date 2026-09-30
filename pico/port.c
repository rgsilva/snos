#include "port.h"
#include <string.h>
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "snoslink.pio.h"
static PIO const pio = pio0;
/* The wire level that reads as "no button": released. Open-drain TX bytes
   are stored inverted (0 bit = release), push-pull ones true (1 = high). */
#ifdef DATA1_OPEN_DRAIN
#define IDLE_BYTE 0x00
#else
#define IDLE_BYTE 0xff
#endif
static uint sm, offset, tx_dma, rx_dma;
static uint8_t pending[LINK_FRAME], tx[LINK_FRAME], rx[LINK_FRAME];
static uint8_t received[4][LINK_FRAME];
static unsigned head, tail;
static bool armed;
static bool idle = true;
static port_stats_t stats;
static port_event_t events[16];
static unsigned nevents;
static uint8_t last_cause;
static void restart(void) {
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_set_irq1_enabled(rx_dma, false);
    /* Read the remaining count BEFORE the abort: an aborted channel reads
       back 0 remaining, which made every burst look like a full frame. */
    unsigned moved = LINK_FRAME - dma_channel_hw_addr(rx_dma)->transfer_count;
    dma_channel_abort(rx_dma); dma_channel_abort(tx_dma);
    if (armed) {
        unsigned bytes = moved;
        if (bytes) {
            stats.clocks = bytes * 8;
            if (bytes == LINK_FRAME) {
                ++stats.complete;
                if (head - tail == 4) ++stats.overruns;
                else memcpy(received[head++ % 4], rx, LINK_FRAME);
            } else ++stats.short_frames;
        }
    }
    ++stats.restarts;
    if (armed) events[nevents++ % 16] = (port_event_t){ time_us_32(), last_cause, (uint8_t)moved };
    pio_sm_clear_fifos(pio, sm); pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset));
    if (idle) memset(tx, IDLE_BYTE, LINK_FRAME); else memcpy(tx, pending, LINK_FRAME);
    armed = true;
    /* Re-arm. A DMA channel keeps DREQ credits across an abort; a stale one
       reads the empty RX FIFO as a zero byte (FDEBUG RXUNDER). The SM is
       disabled here, so any transfer right after the start is exactly that:
       drain and try again. */
    for (int tries = 0; tries < 8; ++tries) {
        pio->fdebug = 1u << (PIO_FDEBUG_RXUNDER_LSB + sm);
        dma_channel_set_read_addr(tx_dma, tx, false);
        dma_channel_set_write_addr(rx_dma, rx, false);
        dma_channel_set_trans_count(tx_dma, LINK_FRAME, false);
        dma_channel_set_trans_count(rx_dma, LINK_FRAME, false);
        dma_hw->ints1 = 1u << rx_dma;
        dma_start_channel_mask((1u << tx_dma) | (1u << rx_dma));
        busy_wait_us(2);
        if (!(pio->fdebug & (1u << (PIO_FDEBUG_RXUNDER_LSB + sm))) &&
            dma_channel_hw_addr(rx_dma)->transfer_count == LINK_FRAME) break;
        ++stats.underruns;
        dma_channel_abort(rx_dma); dma_channel_abort(tx_dma);
        pio_sm_clear_fifos(pio, sm);
    }
    dma_channel_set_irq1_enabled(rx_dma, true);
    pio_sm_set_enabled(pio, sm, true);
    pio_interrupt_clear(pio, 0);
}
static void service(void) {
    uint32_t save = save_and_disable_interrupts();
    bool gap = pio_interrupt_get(pio, 0);
    if (gap || (dma_hw->ints1 & (1u << rx_dma))) {
        if (gap) ++stats.gaps;
        last_cause = gap ? 'g' : 'd';
        restart();
    }
    restore_interrupts(save);
}
void port_init(void) {
    sm = pio_claim_unused_sm(pio, true);
#ifdef DATA1_OPEN_DRAIN
    offset = pio_add_program(pio, &snoslink_od_program);
    snoslink_program_init(pio, sm, offset);
    /* Output value 0, direction from the data: released = pulled up to 5 V. */
    gpio_disable_pulls(5);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << 5);
    pio_sm_set_consecutive_pindirs(pio, sm, 5, 1, false);
#else
    offset = pio_add_program(pio, &snoslink_program);
    snoslink_program_init(pio, sm, offset);
#endif
    tx_dma = dma_claim_unused_channel(true); rx_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true); channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    /* Byte writes replicate across the FIFO word; OUT consumes its high byte. */
    dma_channel_configure(tx_dma, &c, &pio->txf[sm], tx, LINK_FRAME, false);
    c = dma_channel_get_default_config(rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false); channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
    dma_channel_configure(rx_dma, &c, rx, &pio->rxf[sm], LINK_FRAME, false);
    /* Exclusive handlers: the SDK's shared-handler pool holds four entries
       for the whole program, and USB stdio plus a GPIO callback already use
       most of it. Running out is a hard assert during init. */
    irq_set_exclusive_handler(DMA_IRQ_1, service);
    dma_channel_set_irq1_enabled(rx_dma, true); irq_set_enabled(DMA_IRQ_1, true);
    irq_set_exclusive_handler(PIO0_IRQ_0, service);
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
    irq_set_enabled(PIO0_IRQ_0, true);
    restart();
}
void port_set_tx(const uint8_t *frame) {
    uint32_t save = save_and_disable_interrupts();
#ifdef DATA1_OPEN_DRAIN
    for (unsigned i = 0; i < LINK_FRAME; ++i) pending[i] = (uint8_t)~frame[i];
#else
    memcpy(pending, frame, LINK_FRAME);
#endif
    restore_interrupts(save);
}
bool port_take_rx(uint8_t *frame) {
    uint32_t save = save_and_disable_interrupts();
    service();
    bool ready = head != tail;
    if (ready) memcpy(frame, received[tail++ % 4], LINK_FRAME);
    restore_interrupts(save);
    return ready;
}
port_stats_t port_stats(void) {
    uint32_t save = save_and_disable_interrupts();
    port_stats_t result = stats;
    restore_interrupts(save); return result;
}

unsigned port_events(port_event_t *out, unsigned max) {
    uint32_t save = save_and_disable_interrupts();
    unsigned n = nevents < 16 ? nevents : 16;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; ++i) out[i] = events[(nevents - n + i) % 16];
    restore_interrupts(save);
    return n;
}
uint32_t port_padoe(void) { return pio->dbg_padoe; }
uint32_t port_padout(void) { return pio->dbg_padout; }
void port_set_idle(bool want) {
    uint32_t save = save_and_disable_interrupts();
    idle = want;
    restore_interrupts(save);
}
