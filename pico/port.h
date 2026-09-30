#ifndef SNOS_PICO_PORT_H
#define SNOS_PICO_PORT_H
#include "link.h"
typedef struct { uint32_t gaps, restarts, complete, short_frames, clocks, overruns, underruns; } port_stats_t;
void port_init(void);
void port_set_tx(const uint8_t *frame);
/* Idle = look like an empty port: every bit released, so the auto-joypad read
   and any menu polling see no buttons. Start idle; leave it once a valid
   frame has arrived; return to it when the peer goes quiet. */
void port_set_idle(bool idle);
bool port_take_rx(uint8_t *frame);
port_stats_t port_stats(void);
/* The last restarts, newest last: cause 'd' (RX DMA complete) or 'g' (gap),
   bytes the DMA had moved, and the time (us) of the restart. */
typedef struct { uint32_t t; uint8_t cause, bytes; } port_event_t;
unsigned port_events(port_event_t *out, unsigned max);
uint32_t port_padoe(void);
uint32_t port_padout(void);
#endif
