#include <stdio.h>
#include "log.h"
#include <inttypes.h>
#include "pico/stdlib.h"
#include "port.h"
#include "hardware/gpio.h"

/* Burst-start trace: on the first CLK falling edge after arming, sample CLK
   and LATCH every ~1 us for 256 us and print them once. Shows directly what
   the pins do during the first ~30 bits of an exchange. */
#define TRACE_N 256
static volatile bool trace_armed, trace_ready;
static uint8_t trace_bits[TRACE_N];
static void trace_irq(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    if (!trace_armed) return;
    trace_armed = false;
    gpio_set_irq_enabled(2, GPIO_IRQ_EDGE_FALL, false);
    for (int i = 0; i < TRACE_N; ++i) {
        uint32_t v = gpio_get_all();
        trace_bits[i] = (uint8_t)(((v >> 2) & 1) | (((v >> 3) & 1) << 1));
        busy_wait_us(1);
    }
    trace_ready = true;
}
static void trace_arm(void) {
    trace_armed = true;
    gpio_set_irq_enabled(2, GPIO_IRQ_EDGE_FALL, true);
}
static void trace_print(void) {
    char clk[TRACE_N + 1], latch[TRACE_N + 1];
    for (int i = 0; i < TRACE_N; ++i) {
        clk[i] = (trace_bits[i] & 1) ? '1' : '0';
        latch[i] = (trace_bits[i] & 2) ? '1' : '0';
    }
    clk[TRACE_N] = latch[TRACE_N] = 0;
    printf(P_WIRE "trace clk  =%s\n", clk);
    printf(P_WIRE "trace latch=%s\n", latch);
}

int main(void) {
    stdio_init_all();
    uint8_t tx[LINK_FRAME] = {0x80}, last[LINK_FRAME] = {0};
    for (unsigned i = 1; i < LINK_FRAME; ++i) tx[i] = "ABCDEFGHIJ"[(i - 1) % 10];
    port_set_tx(tx); port_init();
    gpio_set_irq_callback(trace_irq);          /* registered once; armed on demand */
    irq_set_enabled(IO_IRQ_BANK0, true);
    uint32_t frames = 0, ok = 0, bad = 0, last_ok_at = 0;
    uint32_t stats_at = to_ms_since_boot(get_absolute_time()) + 1000;
    uint32_t trace_at = stats_at + 2000;
    for (;;) {
        if (trace_ready) { trace_ready = false; trace_print(); }
        if (port_take_rx(last)) {
            bool valid = true;
            ++frames;
            for (unsigned i = 1; i < LINK_FRAME; ++i)
                if (last[i] != "1234567890"[(i - 1) % 10]) valid = false;
            tx[0] = 0x80 | ((tx[0] + 1) & 0x7f); port_set_tx(tx);
            if (valid) { last_ok_at = to_ms_since_boot(get_absolute_time()); port_set_idle(false); }
            if (valid) ++ok;
            else {
                ++bad;
                char hex[LINK_FRAME * 3 + 1];
                for (unsigned i = 0; i < LINK_FRAME; ++i)
                    snprintf(hex + i * 3, 4, "%02x ", last[i]);
                if (bad <= 3 || bad % 600 == 0)
                    printf(P_WIRE "bad=%"PRIu32" clocks=%"PRIu32" hex=%s\n", bad, port_stats().clocks, hex);
            }
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (last_ok_at && (int32_t)(now - last_ok_at) > 2000) { last_ok_at = 0; port_set_idle(true); }
        if ((int32_t)(now - stats_at) >= 0) {
            stats_at = now + 1000;
            if ((int32_t)(now - trace_at) >= 0) { trace_at = now + 5000; trace_arm(); }
            char text[32];
            for (unsigned i = 1; i <= 31; ++i)
                text[i - 1] = last[i] >= 32 && last[i] <= 126 ? last[i] : '.';
            text[31] = 0;
            port_stats_t p = port_stats();
            port_event_t ev[16];
            unsigned n = port_events(ev, 16);
            printf(P_WIRE "events:");
            for (unsigned i = 0; i < n; ++i)
                printf(" %c%u%+ld", ev[i].cause, ev[i].bytes, i ? (long)(ev[i].t - ev[i - 1].t) : 0L);
            printf("\n");
            printf(P_WIRE "pio: padoe=%08lx padout=%08lx (bit5 = DATA1 driven / level)\n",
                   (unsigned long)port_padoe(), (unsigned long)port_padout());
            printf(P_WIRE "frames=%"PRIu32" ok=%"PRIu32" bad=%"PRIu32
                   " short=%"PRIu32" gaps=%"PRIu32" restarts=%"PRIu32
                   " underruns=%"PRIu32" clocks=%"PRIu32" data1=%d snes#=%u last=%s\n",
                   frames, ok, bad, p.short_frames, p.gaps, p.restarts,
                   p.underruns, p.clocks, gpio_get(5), last[0], text);
        }
        tight_loop_contents();
    }
}
