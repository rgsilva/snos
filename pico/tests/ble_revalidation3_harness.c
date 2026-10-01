/* Third-pass tests: production callbacks, real SDK TLV and notification code,
 * and the device-path lock model from the adjudicated second-pass fixture.
 * Events below never silently replace the application's current handle.
 */
#define BLE_REVALIDATION_NO_MAIN
#include "ble_revalidation_harness.c"

static void emit_security(uint8_t event, uint16_t handle, uint8_t status) {
    uint8_t p[15] = {0};
    p[0] = event;
    p[1] = sizeof(p) - 2;
    little_endian_store_16(p, 2, handle);
    p[11] = status;
    sm_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void emit_connection(uint16_t handle) {
    uint8_t p[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE};
    little_endian_store_16(p, 4, handle);
    p[7] = target_addr_type;
    reverse_bd_addr(target_addr, p + 8);
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void emit_disconnect(uint16_t handle) {
    uint8_t p[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 0, 0, 0x13};
    little_endian_store_16(p, 3, handle);
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    if (handle == 0x40) hids_active = false; /* SDK finalizes this client. */
}

static void emit_identity(uint8_t event, uint16_t handle) {
    uint8_t p[20] = {0};
    p[0] = event;
    p[1] = sizeof(p) - 2;
    little_endian_store_16(p, 2, handle);
    p[4] = target_addr_type;
    reverse_bd_addr(target_addr, p + 5);
    p[11] = BD_ADDR_TYPE_LE_PUBLIC;
    reverse_bd_addr(bond_address, p + 12);
    sm_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void setup_link(void) {
    populate_bond_store();
    initialized = radio_on = working = true;
    connecting = true; /* Keep the unrelated reconnect poll out of tests. */
    le_handle = 0x40;
}

static void test_boot_timeout_reconnect_completes_report_delivery(void) {
    setup_link();
    start_le_hids();
    service_event(ERROR_CODE_SUCCESS);
    validation_now += 4999;
    btkbd_poll();
    assert(disconnect_calls == 0);
    ++validation_now;
    btkbd_poll();
    assert(disconnect_calls == 1 && le_prefer_report_mode && !le_boot_waiting);
    emit_disconnect(0x40);
    validation_now += 3000;
    btkbd_poll();
    emit_connection(0x41);
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x41, ERROR_CODE_SUCCESS);
    assert(connect_calls == 2 && le_report_mode);
    service_event(ERROR_CODE_SUCCESS);
    validation_notify(le_hids_event, 1, shift_a);
    check_shift_a();
    assert(connected && !le_pairing_waiting);
}

static void test_deferred_repair_runs_after_sm_reset(void) {
    setup_link();
    sdk_pairing_request_ignored = true;
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_AUTHENTICATION_FAILURE);
    assert(pairing_requests == 0 && le_pair_retry);
    sdk_pairing_request_ignored = false;
    btkbd_poll();
    assert(accepted_pairing_requests == 1 && !le_pair_retry);
    emit_security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_SUCCESS);
    validation_now += 6000;
    btkbd_poll();
    assert(connect_calls == 1 && disconnect_calls == 0);
}

static void test_unstartable_repair_disconnects(void) {
    setup_link();
    sdk_pairing_request_ignored = true;
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_CONNECTION_TIMEOUT);
    btkbd_poll();
    assert(pairing_requests == 1 && accepted_pairing_requests == 0);
    validation_now += 5000;
    btkbd_poll();
    assert(disconnect_calls == 1 && connect_calls == 0);
}

static void passkey_repair(uint8_t failure) {
    setup_link();
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, failure);
    btkbd_poll();
    assert(accepted_pairing_requests == 1);
    emit_security(SM_EVENT_PAIRING_STARTED, 0x40, 0);
    emit_security(SM_EVENT_PASSKEY_DISPLAY_NUMBER, 0x40, 0);
    /* A user taking six seconds to read/type the six-digit passkey is still
     * inside the SDK's 30-second SMP timer (sm.c:676-686). Pairing has started;
     * a watchdog for a request that could not start must not terminate it. */
    validation_now += 6000;
    btkbd_poll();
    assert(disconnect_calls == 0);
    emit_security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1);
}

static void test_missing_key_repair_allows_passkey_entry(void) {
    passkey_repair(ERROR_CODE_PIN_OR_KEY_MISSING);
}
static void test_deferred_repair_allows_passkey_entry(void) {
    passkey_repair(ERROR_CODE_AUTHENTICATION_FAILURE);
}

static void cancel_all(const char *command) {
    setup_link();
    schedule_target_store();
    le_pair_retry = le_pairing_waiting = le_report_retry = le_boot_waiting = true;
    le_hids_started = le_prefer_report_mode = true;
    le_pairing_wait_at = le_boot_wait_at = validation_now;
    assert(btkbd_command(command));
    assert(!target_store_pending && !le_pair_retry && !le_pairing_waiting);
    assert(!le_report_retry && !le_boot_waiting && !le_hids_started);
    assert(le_handle == HCI_CON_HANDLE_INVALID && le_events_cancelled);
    unsigned disconnects = disconnect_calls;
    emit_security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_SUCCESS);
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_AUTHENTICATION_FAILURE);
    service_event(ERROR_CODE_SUCCESS);
    validation_notify(le_hids_event, 1, shift_a);
    validation_now += 6000;
    btkbd_poll();
    assert(pairing_requests == 0 && connect_calls == 0 && reports_seen == 0);
    assert(disconnect_calls == disconnects && !target_store_pending);
}
static void test_off_cancels_all_and_late_security_hids(void) { cancel_all("bt off"); }
static void test_forget_cancels_all_and_late_security_hids(void) { cancel_all("bt forget"); }

static void test_stale_security_handle_ignored(void) {
    setup_link();
    le_handle = 0x41;
    emit_security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_SUCCESS);
    emit_security(SM_EVENT_REENCRYPTION_COMPLETE, 0x40, ERROR_CODE_PIN_OR_KEY_MISSING);
    emit_security(SM_EVENT_PAIRING_COMPLETE, 0x40, ERROR_CODE_AUTHENTICATION_FAILURE);
    assert(connect_calls == 0 && disconnect_calls == 0 && pairing_requests == 0);
}

static void test_off_late_identity_does_not_reschedule_write(void) {
    setup_link();
    assert(btkbd_command("bt off"));
    emit_identity(SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED, 0x40);
    assert(!target_store_pending);
}

static void test_off_late_connection_does_not_restart_pairing(void) {
    setup_link();
    assert(btkbd_command("bt off"));
    emit_connection(0x40); /* Completion already queued when off was typed. */
    assert(le_handle == HCI_CON_HANDLE_INVALID && le_events_cancelled);
    assert(pairing_requests == 0);
}

static void test_forget_late_identity_stays_forgotten(void) {
    setup_link();
    assert(btkbd_command("bt forget"));
    emit_identity(SM_EVENT_IDENTITY_CREATED, 0x40);
    emit_identity(SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED, 0x40);
    emit_connection(0x40);
    btkbd_poll();
    reboot_target_state();
    assert(!target_valid && le_device_db_count() == 0 && pairing_requests == 0);
}

static void test_old_disconnect_does_not_clear_new_link(void) {
    setup_link();
    le_handle = 0x41;
    connected = true;
    connecting = false;
    emit_disconnect(0x40);
    assert(le_handle == 0x41 && connected);
}

static void test_old_hids_cid_does_not_deliver_to_new_link(void) {
    setup_link();
    le_handle = 0x41;
    hids_cid = 2;
    connected = true;
    validation_notify(le_hids_event, 1, shift_a); /* Real dispatcher uses CID 1. */
    assert(reports_seen == 0);
}

static void switch_target_during_setup(void) {
    setup_link();
    start_le_hids();
    service_event(ERROR_CODE_SUCCESS);
    assert(!connected && le_boot_waiting);
    scan_count = 1;
    scan[0].transport = TRANSPORT_LE;
    scan[0].addr_type = BD_ADDR_TYPE_LE_PUBLIC;
    memcpy(scan[0].addr, bond_address, 6);
    scan[0].addr[0] ^= 0x10;
    assert(btkbd_command("bt pair 1"));
}

static void test_switch_target_during_setup_disconnects_old_link(void) {
    switch_target_during_setup();
    /* The old link is live even though it has not delivered its first report.
     * It must be disconnected/cancelled before assigning the new target. */
    assert(disconnect_calls == 1);
    assert(le_handle == HCI_CON_HANDLE_INVALID || le_events_cancelled);
}

static void test_old_keyboard_cannot_claim_replacement_target(void) {
    switch_target_during_setup();
    assert(memcmp(target_addr, scan[0].addr, 6) == 0);
    /* No connection-complete event for the selected new keyboard has arrived.
     * Pressing A on the old still-connected keyboard must not make it ready
     * or deliver input under the replacement target's identity. */
    validation_notify(le_hids_event, HID_BOOT_MODE_KEYBOARD_ID, shift_a);
    assert(!connected && reports_seen == 0 && !target_store_pending);
}

static unsigned store_attempts;
static const btstack_tlv_t *real_tlv;
static int fail_store_once(void *ctx, uint32_t tag, const uint8_t *data, uint32_t len) {
    if (++store_attempts == 1) return 1;
    return real_tlv->store_tag(ctx, tag, data, len);
}
static void test_failed_target_store_retries_and_persists(void) {
    setup_link();
    real_tlv = test_tlv;
    btstack_tlv_t failing_tlv = *real_tlv;
    failing_tlv.store_tag = fail_store_once;
    test_tlv = &failing_tlv;
    strcpy(target_name, "retry saved");
    schedule_target_store();
    btkbd_poll();
    assert(target_store_pending && store_attempts == 1 && validation_context.depth == 0);
    validation_now += 999;
    btkbd_poll();
    assert(store_attempts == 1);
    ++validation_now;
    btkbd_poll();
    assert(!target_store_pending && store_attempts == 2);
    reboot_target_state();
    assert(strcmp(target_name, "retry saved") == 0);
}

/* Release-time callbacks are supported by the SDK, so a new dirty mark must
 * survive the return from the older store. This is an ordering probe; the
 * report distinguishes it from a demonstrated ordinary pairing sequence. */
static void identity_worker(void) {
    emit_identity(SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED, 0x40);
}
static void test_identity_at_lock_release_keeps_new_dirty_record(void) {
    setup_link();
    target_addr_type = BD_ADDR_TYPE_LE_RANDOM;
    target_addr[0] ^= 0x40;
    schedule_target_store();
    pending_worker = identity_worker;
    btkbd_poll();
    assert(memcmp(target_addr, bond_address, 6) == 0);
    /* A second foreground iteration must persist the callback's identity. */
    btkbd_poll();
    reboot_target_state();
    assert(memcmp(target_addr, bond_address, 6) == 0);
}

static void test_boot_timeout_wraps_clock(void) {
    setup_link();
    validation_now = UINT32_MAX - 2000;
    start_le_hids();
    service_event(ERROR_CODE_SUCCESS);
    validation_now += 4999;
    btkbd_poll();
    assert(disconnect_calls == 0);
    ++validation_now;
    btkbd_poll();
    assert(disconnect_calls == 1);
}

static void test_classic_immediate_store_uses_device_lock(void) {
    populate_bond_store();
    classic_opened();
    btkbd_poll();
    assert(lock_acquires == 1 && lock_releases == 1 && !target_store_pending);
    reboot_target_state();
    assert(target_valid && target_transport == TRANSPORT_BR);
}

#define THIRD(name) {#name, test_##name}
static const struct { const char *name; void (*run)(void); } third[] = {
    THIRD(boot_timeout_reconnect_completes_report_delivery),
    THIRD(deferred_repair_runs_after_sm_reset),
    THIRD(unstartable_repair_disconnects),
    THIRD(missing_key_repair_allows_passkey_entry),
    THIRD(deferred_repair_allows_passkey_entry),
    THIRD(off_cancels_all_and_late_security_hids),
    THIRD(forget_cancels_all_and_late_security_hids),
    THIRD(stale_security_handle_ignored),
    THIRD(off_late_identity_does_not_reschedule_write),
    THIRD(off_late_connection_does_not_restart_pairing),
    THIRD(forget_late_identity_stays_forgotten),
    THIRD(old_disconnect_does_not_clear_new_link),
    THIRD(old_hids_cid_does_not_deliver_to_new_link),
    THIRD(switch_target_during_setup_disconnects_old_link),
    THIRD(old_keyboard_cannot_claim_replacement_target),
    THIRD(failed_target_store_retries_and_persists),
    THIRD(identity_at_lock_release_keeps_new_dirty_record),
    THIRD(boot_timeout_wraps_clock),
    THIRD(classic_immediate_store_uses_device_lock),
};

int main(int argc, char **argv) {
    (void)second;
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(third) / sizeof(third[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) puts(third[i].name);
        else if (strcmp(argv[1], third[i].name) == 0) {
            third[i].run();
            return 0;
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
