/* Independent second pass: real ip_service must recover without a test
 * manually sending HELLO or enabling IP. Reuse only the bench's cable/clock
 * boundaries; net.c, ip.c, uIP, link.c and ctrl.c are production objects. */
#define main benchmark_main
#include "linkbench_harness.c"
#undef main

extern int net_reset(void);
extern int ip_ping_send(const uint8_t *dst);
extern volatile uint16_t ip_ping_replies, net_stat_hello;
extern volatile uint8_t net_peer_restarts;

/* The bench's WiFi boundary restates an address on every main-loop turn.
 * Withhold that external event to model DHCP coming up late. */
static bool withhold_address;
void __real_ctrl_address(const uint8_t *ip);
void __wrap_ctrl_address(const uint8_t *ip) {
    if (!withhold_address) __real_ctrl_address(ip);
}

static void ping(void) {
    static const uint8_t gateway_ip[] = {10, 0, 2, 2};
    unsigned before = ip_ping_replies;
    assert(ip_ping_send(gateway_ip) == 0);
    for (unsigned i = 0; i < 180 && ip_ping_replies == before; ++i) kbd_idle();
    assert(ip_ping_replies == (uint16_t)(before + 1));
    assert(ipused == 0);
}

static void adapter_restart(bool delayed_address) {
    /* Stop part-way through a large upload, including a queued wire reply. */
    uint8_t old[1002] = {0xe8, 3};
    assert(net_ip_write(old, sizeof old) == sizeof old);
    for (unsigned i = 0; i < 20 && !ipused; ++i) kbd_idle();
    assert(ipused > 0 && ipused < sizeof old);
    unsigned before = net_peer_restarts;
    withhold_address = delayed_address;
    link_init();
    ctrl_reset();
    nat_core_reset(core);
    nat_core_set_local_address(core, pico_addr());
    ip_stream_reset();
    rxq_count = staged_count = 0;
    rxq_head = staged_head = 0;
    turn_open = false;
    pico_free = now;
    latched_sent = true;
    link_next_out(latched);
    if (!delayed_address) ctrl_address(pico_ip);
    /* No control injection here: ip_service observes the restart counter. */
    for (unsigned i = 0; i < 30 && net_peer_restarts == before; ++i) kbd_idle();
    assert(net_peer_restarts == (uint8_t)(before + 1));
    if (delayed_address) {
        for (unsigned i = 0; i < 300; ++i) kbd_idle();
        assert(!ip_up());
        withhold_address = false;
        ctrl_address(pico_ip);
    }
    for (unsigned i = 0; i < 30 && !ip_up(); ++i) kbd_idle();
    assert(ip_up());
    ping();
}

static void console_reset(void) {
    uint8_t old[1000] = {0x45};
    assert(link_push(CH_IP, old, sizeof old));
    kbd_idle();
    assert(net_reset() >= 0); /* real lifecycle, not session_init alone */
    /* A staged pre-reset frame can alias the first HELLO reply's sequence;
     * allow the production two-second retry rather than injecting HELLO. */
    for (unsigned i = 0; i < 360 && !ip_up(); ++i) kbd_idle();
    assert(ip_up() && net_stat_hello > 0);
    ping();
}

int main(int argc, char **argv) {
    assert(argc == 3);
    /* Initialise all clock costs and exercise an ordinary real-stack TCP
     * transfer first. This does not replace any recovery assertions. */
    char *args[] = {argv[0], "seconds=1", argv[1]};
    assert(benchmark_main(3, args) == 0);
    if (!strcmp(argv[2], "adapter")) adapter_restart(false);
    else if (!strcmp(argv[2], "late-address")) adapter_restart(true);
    else { assert(!strcmp(argv[2], "net-reset")); console_reset(); }
    printf("PASS real ip_service %s %s: automatic HELLO and fresh ping\n", argv[1], argv[2]);
    return 0;
}
