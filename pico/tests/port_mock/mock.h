/* SDK boundary for port_validation_harness.c. No GPIO or timing emulation. */
#ifndef PORT_MOCK_H
#define PORT_MOCK_H
#include <stdbool.h>
#include <stdint.h>
typedef unsigned uint;
typedef struct { uint32_t fdebug, txf[4], rxf[4], dbg_padoe, dbg_padout; } mock_pio;
static mock_pio pio_storage;
typedef mock_pio *PIO;
#define pio0 (&pio_storage)
static struct { uint32_t ints1; } dma_storage;
#define dma_hw (&dma_storage)
static struct { uint32_t transfer_count; } channels[2];
#define dma_channel_hw_addr(n) (&channels[n])
typedef int dma_channel_config;
#define PIO_FDEBUG_RXUNDER_LSB 0
#define DMA_IRQ_1 1
#define PIO0_IRQ_0 0
#define DMA_SIZE_8 0
#define pis_interrupt0 0
static unsigned allocated;
static bool gap_pending;
static unsigned mock_us;
static int snoslink_program, snoslink_od_program;
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }
static inline void pio_sm_set_enabled(PIO p, uint s, bool b) { (void)p;(void)s;(void)b; }
static inline void dma_channel_set_irq1_enabled(uint n, bool b) { (void)n;(void)b; }
static inline void dma_channel_abort(uint n) { channels[n].transfer_count = 0; }
static inline void pio_sm_clear_fifos(PIO p, uint s) { (void)p;(void)s; }
static inline void pio_sm_restart(PIO p, uint s) { (void)p;(void)s; }
static inline void pio_sm_exec(PIO p, uint s, uint i) { (void)p;(void)s;(void)i; }
static inline uint pio_encode_jmp(uint n) { return n; }
static inline void dma_channel_set_read_addr(uint n, const void *p, bool b) { (void)n;(void)p;(void)b; }
static inline void dma_channel_set_write_addr(uint n, void *p, bool b) { (void)n;(void)p;(void)b; }
static inline void dma_channel_set_trans_count(uint n, uint c, bool b) {
    (void)b; channels[n].transfer_count = c;
}
static inline void dma_start_channel_mask(uint m) {
    (void)m;
    /* Model W1C registers after firmware writes them, and no stale DREQ. */
    pio_storage.fdebug = 0; dma_storage.ints1 = 0;
}
static inline void busy_wait_us(uint n) { mock_us += n; }
static inline void pio_interrupt_clear(PIO p, uint n) { (void)p;(void)n;gap_pending=false; }
static inline bool pio_interrupt_get(PIO p, uint n) { (void)p;(void)n;return gap_pending; }
static inline uint pio_claim_unused_sm(PIO p, bool b) { (void)p;(void)b;return 0; }
static inline uint pio_add_program(PIO p, const void *q) { (void)p;(void)q;return 0; }
static inline void snoslink_program_init(PIO p, uint s, uint o) { (void)p;(void)s;(void)o; }
static inline void gpio_disable_pulls(uint n) { (void)n; }
static inline void pio_sm_set_pins_with_mask(PIO p, uint s, uint v, uint m) {
    (void)p;(void)s;(void)v;(void)m;
}
static inline void pio_sm_set_consecutive_pindirs(PIO p, uint s, uint b, uint n, bool v) {
    (void)p;(void)s;(void)b;(void)n;(void)v;
}
static inline uint dma_claim_unused_channel(bool b) { (void)b;return allocated++; }
static inline dma_channel_config dma_channel_get_default_config(uint n) { (void)n;return 0; }
static inline void channel_config_set_transfer_data_size(dma_channel_config *c, uint n) { (void)c;(void)n; }
static inline void channel_config_set_read_increment(dma_channel_config *c, bool b) { (void)c;(void)b; }
static inline void channel_config_set_write_increment(dma_channel_config *c, bool b) { (void)c;(void)b; }
static inline void channel_config_set_dreq(dma_channel_config *c, uint n) { (void)c;(void)n; }
static inline uint pio_get_dreq(PIO p, uint s, bool b) { (void)p;(void)s;(void)b;return 0; }
static inline void dma_channel_configure(uint n, dma_channel_config *c, void *w,
                                         const void *r, uint count, bool b) {
    (void)c;(void)w;(void)r;(void)b;channels[n].transfer_count=count;
}
static inline void irq_set_exclusive_handler(uint n, void (*f)(void)) { (void)n;(void)f; }
static inline void irq_set_enabled(uint n, bool b) { (void)n;(void)b; }
static inline void pio_set_irq0_source_enabled(PIO p, uint n, bool b) { (void)p;(void)n;(void)b; }
static inline uint32_t time_us_32(void) { return mock_us; }
#endif
