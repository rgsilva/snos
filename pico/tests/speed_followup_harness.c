/* Deliberately adversarial scheduler probes, excluded from *_test.py. These
 * demonstrate limits of the universal fairness claim, not normal-use failures. */
#define main original_nat_harness_main
#include "nat_harness.c"
#undef main

static bool occupied;
static bool packet_slot(void *arg, const uint8_t *p, size_t len) {
    if (occupied) return false;
    emit(arg, p, len); occupied = true; return true;
}
static bool link_busy_probe(void *arg) { (void)arg; return occupied; }
static uint32_t remote_open(unsigned slot, uint16_t port) {
    send_tcp_to(REMOTE, port, 80, SYN, 100, 0, 1460, NULL);
    nat_core_tcp_connected(n, slot, 0, tick);
    uint32_t ack = rd32(last + 24) + 1;
    send_tcp_to(REMOTE, port, 80, ACK, 101, ack, 1460, NULL);
    return ack;
}
static int ack_preemption(bool delayed_write) {
    uint32_t sender = remote_open(0, 5700);
    remote_open(1, 5701);
    assert(nat_core_tcp_received(n, 1, (const uint8_t *)"victim", 6, tick));
    nat_core_set_adapter(n, packet_slot, link_busy_probe, NULL, NULL);
    unsigned received = 0;
    for (unsigned opportunity = 0; opportunity < 1000; ++opportunity) {
        tick += 10;
        occupied = false;
        /* Force a fresh one-byte upload at EVERY free packet opportunity.
         * For poll ACKs, also force tcp_write backpressure before each poll. */
        write_ok = !delayed_write;
        send_tcp_to(REMOTE, 5700, 80, PSH | ACK, 101 + opportunity, sender, 1460, "u");
        write_ok = true;
        nat_core_poll(n, tick);
        assert(occupied);
        received += rd16(last + 22) == 5701;
        assert(last[33] == ACK || rd16(last + 22) == 5701);
    }
    printf("%s: victim packets=%u across 1000 forced ACK-preempted opportunities\n",
           delayed_write ? "poll ACK" : "input ACK", received);
    return received ? 0 : 1;
}
static int sparse_busy_poll(void) {
    uint32_t ack = remote_open(0, 5700);
    assert(nat_core_tcp_received(n, 0, (const uint8_t *)"retry", 5, tick));
    nat_core_set_adapter(n, packet_slot, link_busy_probe, NULL, NULL);
    nat_core_poll(n, tick);
    unsigned before = emitted;
    for (unsigned opportunity = 0; opportunity < 1000; ++opportunity) {
        tick += 2501;
        occupied = true; /* never observe busy within the previous RTO */
        send_tcp_to(REMOTE, 5700, 80, ACK, 101, ack, 1460, NULL);
        nat_core_poll(n, tick);
        occupied = false;
        nat_core_poll(n, ++tick);
    }
    printf("sparse busy poll: retries=%u across 1000 artificially sparse RTO cycles\n",
           emitted - before);
    return emitted > before ? 0 : 1;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    nat_ops_t ops = {emit, connect_, write_, shutdown_, abort_, udp, fadd, fdel, got, NULL};
    nat_core_init(n, &ops);
    if (!strcmp(argv[1], "input-ack")) return ack_preemption(false);
    if (!strcmp(argv[1], "poll-ack")) return ack_preemption(true);
    if (!strcmp(argv[1], "sparse-busy")) return sparse_busy_poll();
    return 2;
}
