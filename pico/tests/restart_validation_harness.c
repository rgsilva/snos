/* A real adapter restart is different from a NAT/relay mode switch. The
 * surviving console must be able to start a fresh IP stream afterwards.
 * Capture one console TX frame just as port.c stages it on the wire. */
#define TRANSITION_NO_MAIN 1
#include "transition_harness.c"

static void restart_mid_upload(void) {
    setup_peer();
    uint8_t old[1002]; stream_bytes(old, 1000, 13);
    /* Valid binary payload. At the next link-frame boundary its bytes also
       happen to spell a plausible length prefix (512) without the header. */
    old[58] = 0; old[59] = 2;
    old[24] = old[25] = 0;
    put16(old + 24, checksum(old + 22, 980, 0));
    assert(console_peer_ip_write(old, sizeof old) == sizeof old);
    pump(1);
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    link_next_out(f); console_peer_receive(f, reply);
    /* Firmware restarts after the first upload fragment, before accepting
       the already-staged second one. No delayed/reordered ancient frames. */
    link_init(); ctrl_reset(); nat_reset(); ip_stream_reset();
    accept_reply(reply); pump(30);
    unsigned hellos = console_peer_hellos();
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    ctrl_address((const uint8_t *)"\xc0\0\2\x0a"); pump(20);
    assert(console_peer_hellos() == hellos + 1);
    assert(!strcmp(console_peer_address(), "192.0.2.10"));
    unsigned stale = ipused;
    uint8_t fresh[42]; stream_bytes(fresh, 40, 91);
    console_peer_enable_ip();
    received_len = reverse_len = 0;
    assert(console_peer_ip_write(fresh, sizeof fresh) == sizeof fresh);
    pump(80);
    fprintf(stderr, "restart: stale parser=%u, fresh wire=%zu, echo=%zu, parser now=%zu\n",
            stale, reverse_len, received_len, ipused);
    assert(reverse_len == sizeof fresh);
    assert(received_len == sizeof fresh);
    assert(ipused == 0);
}
int main(int argc, char **argv) {
    signal(SIGABRT, assertion_exit);
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        puts("restart_mid_upload_recovers_fresh_ping"); return 0;
    }
    char *setup[] = {argv[0], "--setup-only"};
    assert(baseline_main(2, setup) == 2);
    restart_mid_upload();
    return 0;
}
