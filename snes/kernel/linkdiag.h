/* LINKDIAG=1 (hardware images only): counters on the cable's way from the
 * NMI to the clocks, shown on screen about once a second, for a console the
 * owner can boot and photograph but not type on (docs/sa1-notes.md).
 *
 * The NMI and the link only store numbers; the foreground formats them
 * (kbd_idle). Without LINKDIAG every hook below is empty and the image is
 * byte-identical to a build without this file. */
#ifndef SNOS_LINKDIAG_H
#define SNOS_LINKDIAG_H

#include <stdint.h>

#ifdef LINKDIAG

/* Called from the NMI: first thing, after pad_latch, before and after
 * net_service, last thing. */
void ld_nmi_entry(void);
void ld_nmi_pad(void);
void ld_nmi_exit(void);
extern uint8_t ld_net_ran;  /* this NMI ran net_service */
extern uint16_t ld_guard;   /* pad_latch's guard as it left */

/* From link_serial.c: a probe reached xfer; the gate read `line`; it
 * refused; the clocks started; a reply checksummed. */
void ld_probe(void);
void ld_gate(uint8_t line);
void ld_refused(uint8_t line);
void ld_clocked(void);
void ld_rx_ok(const uint8_t *frame);

/* From kbd_idle, foreground only: repaint the panel when a second is due. */
void ld_show(void);

/* Read-only views of state the diagnostic reports. */
int link_backend(void);
int net_is_ready(void);
int link_serial_diag(uint16_t *probe_wait);

#define LD_NMI_ENTRY() ld_nmi_entry()
#define LD_NMI_PAD() ld_nmi_pad()
#define LD_NMI_EXIT() ld_nmi_exit()
#define LD_NET_RAN() (ld_net_ran = 1)
#define LD_GUARD(g) (ld_guard = (g))
#define LD_PROBE() ld_probe()
#define LD_GATE(l) ld_gate(l)
#define LD_REFUSED(l) ld_refused(l)
#define LD_CLOCKED() ld_clocked()
#define LD_RX_OK(f) ld_rx_ok(f)
#define LD_SHOW() ld_show()

#else

#define LD_NMI_ENTRY()
#define LD_NMI_PAD()
#define LD_NMI_EXIT()
#define LD_NET_RAN()
#define LD_GUARD(g)
#define LD_PROBE()
#define LD_GATE(l)
#define LD_REFUSED(l)
#define LD_CLOCKED()
#define LD_RX_OK(f)
#define LD_SHOW()

#endif
#endif
