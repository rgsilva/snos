/* Fourth-pass lifecycle coverage. Reuse the lock-aware fixture unchanged.
 * SDK effects remain spies; notification formatting and TLV storage are real.
 * The two explicitly selected follow-up probes require synthetic event order.
 */
#define BLE_REVALIDATION_NO_MAIN
#include "ble_revalidation_harness.c"

static unsigned cancel_calls;
uint8_t gap_connect_cancel(void) {
    ++cancel_calls;
    return ERROR_CODE_SUCCESS;
}

static void setup(void) {
    populate_bond_store();
    initialized = radio_on = working = true;
    connecting = true;
    le_handle = 0x40;
}

static void security(uint8_t event, uint16_t handle, uint8_t status) {
    uint8_t p[15] = {0};
    p[0] = event;
    p[1] = sizeof(p) - 2;
    little_endian_store_16(p, 2, handle);
    p[11] = status;
    sm_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void connection(uint16_t handle, uint8_t status, const bd_addr_t addr) {
    uint8_t p[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE};
    p[3] = status;
    little_endian_store_16(p, 4, handle);
    p[7] = target_addr_type;
    reverse_bd_addr(addr, p + 8);
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void disconnected(uint16_t handle) {
    uint8_t p[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 0, 0, 0x13};
    little_endian_store_16(p, 3, handle);
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void service(uint16_t cid, uint8_t status) {
    uint8_t p[8] = {HCI_EVENT_GATTSERVICE_META, 6,
                   GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED};
    little_endian_store_16(p, 3, cid);
    p[5] = status;
    le_hids_event(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void replacement(transport_t transport) {
    scan_count = 1;
    scan[0].transport = transport;
    scan[0].addr_type = transport == TRANSPORT_LE ? BD_ADDR_TYPE_LE_PUBLIC : BD_ADDR_TYPE_ACL;
    memcpy(scan[0].addr, bond_address, 6);
    scan[0].addr[0] ^= 0x10;
    strcpy(scan[0].name, "replacement");
    assert(btkbd_command("bt pair 1"));
}

static void progress(uint8_t event) {
    setup();
    security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_PIN_OR_KEY_MISSING);
    assert(le_pairing_waiting);
    security(event, 0x40, 0);
    assert(!le_pairing_waiting);
    validation_now += 25000;
    btkbd_poll();
    assert(!disconnect_calls && !connect_calls);
    /* The application must still honor the SDK's eventual timeout/failure. */
    security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_CONNECTION_TIMEOUT);
    assert(disconnect_calls == 1 && !connect_calls);
}
static void started_alone(void) { progress(SM_EVENT_PAIRING_STARTED); }
static void passkey_alone(void) { progress(SM_EVENT_PASSKEY_DISPLAY_NUMBER); }
static void just_works_alone(void) { progress(SM_EVENT_JUST_WORKS_REQUEST); }

static void stale_progress_keeps_watchdog(void) {
    setup();
    security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_PIN_OR_KEY_MISSING);
    security(SM_EVENT_PAIRING_STARTED, 0x41, 0);
    security(SM_EVENT_PASSKEY_DISPLAY_NUMBER, 0x41, 0);
    security(SM_EVENT_JUST_WORKS_REQUEST, 0x41, 0);
    assert(le_pairing_waiting);
    validation_now += 5000;
    btkbd_poll();
    assert(disconnect_calls == 1 && !connect_calls);
}

static void stale_service_cid_ignored(void) {
    setup();
    start_le_hids();
    /* The older spy omits the output CID. Actual hids_client_connect assigns
     * it before discovery (installed hids_client.c:1458-1459). Model that. */
    hids_cid = 2;
    service(1, ERROR_CODE_SUCCESS);
    service(1, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    assert(!le_boot_waiting && !le_report_retry && !connected);
    service(2, ERROR_CODE_SUCCESS);
    assert(le_boot_waiting && hids_cid == 2);
}

static void switch_completes_delivery_and_persistence(void) {
    setup();
    start_le_hids();
    service(1, ERROR_CODE_SUCCESS);
    replacement(TRANSPORT_LE);
    assert(disconnect_calls == 1 && le_events_cancelled && !hids_cid);
    disconnected(0x40);
    hids_active = false; /* SDK finalizes the old client on disconnect. */
    assert(connecting && le_handle == HCI_CON_HANDLE_INVALID);
    connection(0x41, ERROR_CODE_SUCCESS, target_addr);
    security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1);
    security(SM_EVENT_PAIRING_COMPLETE, 0x41, ERROR_CODE_SUCCESS);
    hids_cid = 2; /* As assigned by the actual SDK, above. */
    service(1, ERROR_CODE_SUCCESS);
    assert(!le_boot_waiting);
    service(2, ERROR_CODE_SUCCESS);
    /* The SDK notification fixture emits CID 1. Adapt only that routing field
     * below; the notification payload still comes from the SDK dispatcher. */
}

static void notify_cid2(uint8_t type, uint16_t channel, uint8_t *p, uint16_t size) {
    little_endian_store_16(p, 3, 2);
    le_hids_event(type, channel, p, size);
}
static void complete_switch(void) {
    switch_completes_delivery_and_persistence();
    validation_notify(le_hids_event, 1, shift_a);
    assert(reports_seen == 0 && !connected);
    validation_notify(notify_cid2, 1, shift_a);
    check_shift_a();
    btkbd_poll();
    reboot_target_state();
    assert(target_valid && same_addr(target_addr, scan[0].addr));
    assert(strcmp(target_name, "replacement") == 0);
}

static void switch_pending_connect_cancels(void) {
    setup();
    le_handle = HCI_CON_HANDLE_INVALID;
    replacement(TRANSPORT_LE);
    assert(cancel_calls == 1 && !disconnect_calls && connecting);
    connection(0x41, ERROR_CODE_SUCCESS, target_addr);
    assert(le_handle == 0x41 && !le_events_cancelled && pairing_requests == 1);
}

static void forget_pending_connect_cancels(void) {
    setup();
    le_handle = HCI_CON_HANDLE_INVALID;
    assert(btkbd_command("bt forget"));
    assert(cancel_calls == 1 && !target_valid && !connecting);
    connection(0x40, ERROR_CODE_SUCCESS, bond_address);
    btkbd_poll();
    reboot_target_state();
    assert(!target_valid && !pairing_requests && !le_device_db_count());
}

static void failed_connection_retries(void) {
    setup();
    le_handle = HCI_CON_HANDLE_INVALID;
    connection(0x40, ERROR_CODE_CONNECTION_TIMEOUT, target_addr);
    assert(!connecting && !pairing_requests && le_handle == HCI_CON_HANDLE_INVALID);
    validation_now += 4999;
    btkbd_poll();
    assert(!connecting);
    ++validation_now;
    btkbd_poll();
    assert(connecting);
    connection(0x41, ERROR_CODE_SUCCESS, target_addr);
    assert(le_handle == 0x41 && pairing_requests == 1);
}

static void radio_state(uint8_t state) {
    uint8_t p[3] = {BTSTACK_EVENT_STATE, 1, state};
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void off_on_reconnect(void) {
    setup();
    scanning = scan_print_pending = true;
    assert(btkbd_command("bt off"));
    radio_state(HCI_STATE_WORKING);
    connection(0x40, ERROR_CODE_SUCCESS, target_addr);
    security(SM_EVENT_IDENTITY_CREATED, 0x40, 0);
    service(1, ERROR_CODE_SUCCESS);
    assert(!working && !scanning && !scan_print_pending && !target_store_pending);
    assert(!pairing_requests && le_events_cancelled);
    assert(btkbd_command("bt on"));
    radio_state(HCI_STATE_WORKING);
    validation_now += 500;
    btkbd_poll();
    assert(connecting);
    connection(0x41, ERROR_CODE_SUCCESS, target_addr);
    security(SM_EVENT_REENCRYPTION_COMPLETE, 0x41, ERROR_CODE_SUCCESS);
    service(1, ERROR_CODE_SUCCESS);
    validation_notify(le_hids_event, 1, shift_a);
    check_shift_a();
}

static void classic_replacement_persists(void) {
    setup();
    replacement(TRANSPORT_BR);
    assert(br_connects == 1 && target_transport == TRANSPORT_BR);
    classic_opened();
    btkbd_poll();
    reboot_target_state();
    assert(target_valid && target_transport == TRANSPORT_BR && !target_store_pending);
}

static void newer_record(void) {
    strcpy(target_name, "newest identity");
    schedule_target_store();
    schedule_target_store();
}
static void generation_wrap_keeps_dirty_record(void) {
    setup();
    target_store_generation = UINT32_MAX - 1;
    schedule_target_store();
    pending_worker = newer_record;
    btkbd_poll();
    assert(target_store_pending && target_store_generation == 1);
    btkbd_poll();
    assert(!target_store_pending);
    reboot_target_state();
    assert(strcmp(target_name, "newest identity") == 0);
}

/* Nonblocking ordering probes, excluded from --list and *_test.py execution.
 * Neither establishes a normal controller/SDK trace producing this order. */
static void followup_service_before_current_hids_start(void) {
    setup();
    replacement(TRANSPORT_LE);
    connection(0x41, ERROR_CODE_SUCCESS, target_addr);
    assert(!hids_cid && !le_hids_started);
    service(1, ERROR_CODE_SUCCESS); /* Hypothetically queued old client. */
    validation_notify(le_hids_event, 1, shift_a);
    assert(!connected && !reports_seen && !target_store_pending);
}
static void followup_old_connect_after_replacement_request(void) {
    setup();
    le_handle = HCI_CON_HANDLE_INVALID;
    bd_addr_t old;
    memcpy(old, target_addr, 6);
    replacement(TRANSPORT_LE);
    connection(0x40, ERROR_CODE_SUCCESS, old); /* Cancel/success race. */
    assert(le_handle == HCI_CON_HANDLE_INVALID && !pairing_requests);
}

#define CASE(name) {#name, name}
static const struct { const char *name; void (*run)(void); } cases[] = {
    CASE(started_alone), CASE(passkey_alone), CASE(just_works_alone),
    CASE(stale_progress_keeps_watchdog), CASE(stale_service_cid_ignored),
    CASE(complete_switch), CASE(switch_pending_connect_cancels),
    CASE(forget_pending_connect_cancels), CASE(failed_connection_retries),
    CASE(off_on_reconnect), CASE(classic_replacement_persists),
    CASE(generation_wrap_keeps_dirty_record),
}, followups[] = {
    CASE(followup_service_before_current_hids_start),
    CASE(followup_old_connect_after_replacement_request),
};

int main(int argc, char **argv) {
    (void)second;
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) puts(cases[i].name);
        else if (strcmp(argv[1], cases[i].name) == 0) { cases[i].run(); return 0; }
    }
    for (size_t i = 0; i < sizeof(followups) / sizeof(followups[0]); ++i) {
        if (strcmp(argv[1], "--followups") == 0) puts(followups[i].name);
        else if (strcmp(argv[1], followups[i].name) == 0) { followups[i].run(); return 0; }
    }
    return strcmp(argv[1], "--list") && strcmp(argv[1], "--followups") ? 2 : 0;
}
