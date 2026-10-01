/* Generated include is actual net.c plus deterministic interrupt cut points.
 * This models the Calypsi instruction interleaving, not CPU timing. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static void interrupt_cut(unsigned phase);
static int nmi_enabled = 1;
#include "console_peer.c"
static unsigned lock_depth, put_number, cut_phase, cut_put;
static int armed;
static uint8_t reply[LINK_FRAME];
/* Actual proc_lock disables NMI just while changing p_lock, then enables it
 * before returning (proc.c:246-251); it does not guard the caller's body. */
void proc_lock(void) { ++lock_depth; nmi_enabled = 1; }
void proc_unlock(void) { assert(lock_depth); --lock_depth; }
static void deliver_restart(void) {
    armed = 0;
    uint8_t f[LINK_FRAME]; memset(f, LINK_CH_PAD, sizeof(f));
    f[LINK_SEQ] = 9; f[LINK_ACK] = 255; f[LINK_FLAGS] = LINK_F_SYN;
    f[LINK_SUM] = net_checksum(f, LINK_FRAME);
    console_peer_receive(f, reply); /* Complete real NMI receive/build/send. */
}
static void interrupt_cut(unsigned phase) {
    if (!armed) return;
    if (phase == 0) ++put_number;
    if (phase != cut_phase || put_number != cut_put || !nmi_enabled) return;
    assert(lock_depth); /* Scheduling is pinned, but NMI remains enabled. */
    deliver_restart();
}
static void run(unsigned phase, unsigned put, uint16_t start) {
    console_peer_init(); txq_head = txq_tail = start;
    cut_phase = phase; cut_put = put; armed = 1;
    assert(net_hello() == 0); assert(!lock_depth);
    /* A future whole-record NMI guard delays this interrupt safely. */
    if (armed) { assert(nmi_enabled); deliver_restart(); }
    unsigned queued = (txq_head - txq_tail) & (TXQ_SIZE - 1);
    fprintf(stderr, "cut=%u put=%u start=%u: head=%u tail=%u queued=%u frame=%u\n",
            phase, put, start, txq_head, txq_tail, queued, txframe_bytes);
    /* The emitted HELLO reserves two ring bytes until ACK. Any interrupted
     * old record may be dropped, but cannot remove its reservation or leave
     * an orphan byte after it. Whole additional HELLO records are allowed
     * if a future implementation preserves or retries the foreground record. */
    assert(reply[4] == LINK_CH_CTRL && reply[5] == 2);
    assert(reply[6] == C2S_HELLO && reply[7] == 0);
    unsigned reserved = txframe_bytes;
    int complete = queued >= reserved && queued % 2 == 0;
    for (unsigned i = 0; i + 1 < queued; i += 2)
        if (txq[(txq_tail + i) & (TXQ_SIZE - 1)] != C2S_HELLO ||
            txq[(txq_tail + i + 1) & (TXQ_SIZE - 1)] != 0) complete = 0;
    uint8_t ack[LINK_FRAME]; memset(ack, LINK_CH_PAD, sizeof(ack));
    ack[LINK_SEQ] = 9; ack[LINK_ACK] = reply[LINK_SEQ]; ack[LINK_FLAGS] = 0;
    ack[LINK_SUM] = net_checksum(ack, LINK_FRAME);
    console_peer_receive(ack, reply);
    fprintf(stderr, "after ACK: queued=%u, next control bytes=%u\n",
            (txq_head - txq_tail) & (TXQ_SIZE - 1), txframe_bytes);
    assert(complete);
    assert(((txq_head - txq_tail) & (TXQ_SIZE - 1)) == queued - reserved);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        puts("stale_head_store\nstale_head_store_wrap\nbyte_store_before_head_reload\nsplit_hello_record"); return 0;
    }
    if (!strcmp(argv[1], "stale_head_store")) run(2, 2, 0);
    else if (!strcmp(argv[1], "stale_head_store_wrap")) run(2, 2, 511);
    else if (!strcmp(argv[1], "byte_store_before_head_reload")) run(1, 2, 0);
    else if (!strcmp(argv[1], "split_hello_record")) run(3, 1, 0);
    else return 2;
    printf("PASS %s\n", argv[1]); return 0;
}
