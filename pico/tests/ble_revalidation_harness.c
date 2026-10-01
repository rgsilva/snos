/* Second-pass tests reuse, but never edit, the first validator's fixtures.
 * Each scenario runs in a fresh process. Radio effects are spies; notification
 * formatting and bond storage execute the installed BTstack implementation.
 */
#define main first_validation_main
#include "ble_validation_harness.c"
#undef main

uint32_t validation_now = 100;
static unsigned pairing_requests, accepted_pairing_requests, resolving_reloads, br_connects;
static bool sdk_pairing_request_ignored;
void sm_request_pairing(hci_con_handle_t handle) {
    assert(handle != HCI_CON_HANDLE_INVALID);
    ++pairing_requests;
    if (!sdk_pairing_request_ignored) ++accepted_pairing_requests;
}
uint8_t gap_load_resolving_list_from_le_device_db(void) {
    ++resolving_reloads;
    return ERROR_CODE_SUCCESS;
}
uint8_t hid_host_connect(bd_addr_t addr, hid_protocol_mode_t mode, uint16_t *cid) {
    (void)addr; (void)cid;
    assert(mode == HID_PROTOCOL_MODE_BOOT);
    ++br_connects;
    return ERROR_CODE_SUCCESS;
}
void hid_host_disconnect(uint16_t cid) { (void)cid; }
int hci_power_control(HCI_POWER_MODE mode) { (void)mode; return 0; }
void gap_set_scan_params(uint8_t type, uint16_t interval, uint16_t window, uint8_t policy) {
    (void)type; (void)interval; (void)window; (void)policy;
}
void gap_set_scan_duplicate_filter(bool enabled) { (void)enabled; }
void gap_start_scan(void) {}
void gap_stop_scan(void) {}
int gap_inquiry_start(uint8_t duration) { (void)duration; return 0; }
int gap_inquiry_stop(void) { return 0; }
int gap_remote_name_request(const bd_addr_t addr, uint8_t mode, uint16_t clock) {
    (void)addr; (void)mode; (void)clock; return 0;
}
int gap_pin_code_response(const bd_addr_t addr, const char *pin) {
    (void)addr; (void)pin; return 0;
}
int gap_ssp_confirmation_response(const bd_addr_t addr) { (void)addr; return 0; }
uint8_t hid_host_accept_connection(uint16_t cid, hid_protocol_mode_t mode) {
    (void)cid; (void)mode; return 0;
}
uint8_t hid_host_decline_connection(uint16_t cid) { (void)cid; return 0; }

bool gap_get_link_key_for_bd_addr(bd_addr_t addr, link_key_t key, link_key_type_t *type) {
    return classic_db ? classic_db->get_link_key(addr, key, type) : 0;
}

static void service_event(uint8_t status) {
    uint8_t p[8] = {HCI_EVENT_GATTSERVICE_META, 6,
        GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED, 1, 0, status};
    le_hids_event(HCI_EVENT_PACKET, 0, p, sizeof(p));
}

static void trace_notification(uint8_t type, uint16_t channel,
                               uint8_t *p, uint16_t size) {
    printf("SDK callback type=%02x channel=%u size=%u:", type, channel, size);
    for (unsigned i = 0; i < size; ++i) printf(" %02x", p[i]);
    puts("");
    le_hids_event(type, channel, p, size);
}

static void test_real_boot_dispatch_after_service_success(void) {
    le_handle = 0x40;
    start_le_hids();
    service_event(ERROR_CODE_SUCCESS);
    assert(!connected && le_boot_waiting);
    validation_notify(trace_notification, HID_BOOT_MODE_KEYBOARD_ID, shift_a);
    assert(connected && !le_boot_waiting);
    check_shift_a();
}

static void test_deferred_retry_completes_real_report_delivery(void) {
    le_handle = 0x40;
    start_le_hids();
    service_event(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    assert(connect_calls == 1 && le_report_retry);
    hids_active = false; /* SDK finalizes after the failure callback returns. */
    btkbd_poll();
    assert(connect_calls == 2 && le_report_mode && !le_report_retry);
    service_event(ERROR_CODE_SUCCESS);
    validation_notify(trace_notification, 1, shift_a);
    check_shift_a();
    btkbd_poll();
    assert(connect_calls == 2 && disconnect_calls == 0);
}

static void test_boot_success_without_input_eventually_recovers(void) {
    le_handle = 0x40;
    start_le_hids();
    /* SDK can emit success without a usable boot input (hids_client.c:961,
     * 1368). A report-only keyboard then cannot produce a boot notification. */
    service_event(ERROR_CODE_SUCCESS);
    for (validation_now = 100; validation_now < 60100; validation_now += 100)
        btkbd_poll();
    assert(le_report_mode || le_report_retry || disconnect_calls);
}

static void test_missing_key_requests_pairing_then_success_starts_hid(void) {
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_PIN_OR_KEY_MISSING);
    assert(pairing_requests == 1 && connect_calls == 0 && disconnect_calls == 0);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1 && disconnect_calls == 0);
}

static void test_repair_failure_disconnects_without_hid(void) {
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_PIN_OR_KEY_MISSING);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_AUTHENTICATION_FAILURE);
    assert(pairing_requests == 1 && connect_calls == 0 && disconnect_calls == 1);
}

static void test_smp_abort_during_reencryption_does_not_stall(void) {
    /* SDK sm_channel_handler, sm.c:5021-5025, emits R(failure) before changing
     * SM_PH4_W4_CONNECTION_ENCRYPTED to SM_INITIATOR_CONNECTED. Its
     * sm_request_pairing:5311-5344 ignores requests in that waiting state.
     * sm_pairing_complete:665 emits nothing when pairing_active is false,
     * as on an ordinary bonded re-encryption (sm.c:2929-2947).
     * Model those SDK side effects, without claiming to emulate the radio. */
    sdk_pairing_request_ignored = true;
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_AUTHENTICATION_FAILURE);
    sdk_pairing_request_ignored = false;
    for (validation_now = 100; validation_now < 60100; validation_now += 100)
        btkbd_poll();
    assert(connect_calls == 0);
    assert(disconnect_calls || accepted_pairing_requests);
}

static void test_pairing_then_reencryption_success_starts_once(void) {
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_SUCCESS);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1 && disconnect_calls == 0);
}

static void test_security_success_during_deferred_retry_keeps_retry(void) {
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    service_event(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_SUCCESS);
    assert(le_report_retry && connect_calls == 1 && disconnect_calls == 0);
    hids_active = false;
    btkbd_poll();
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(le_report_mode && connect_calls == 2 && disconnect_calls == 0);
}

static void test_disconnect_cancels_retry_and_new_link_starts_hid(void) {
    target_transport = TRANSPORT_LE;
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    service_event(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    uint8_t p[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 0x40, 0, 0x13};
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    hids_active = false;
    btkbd_poll();
    assert(!le_report_retry && connect_calls == 1);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(connect_calls == 2 && !le_report_mode);
}

static void test_retry_does_not_run_after_radio_off(void) {
    initialized = radio_on = working = true;
    le_handle = 0x40;
    start_le_hids();
    service_event(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    hids_active = false;
    assert(btkbd_command("bt off")); /* Before async disconnection event. */
    btkbd_poll();
    assert(connect_calls == 1);
}

static void test_retry_does_not_run_after_forget(void) {
    initialized = true;
    le_handle = 0x40;
    start_le_hids();
    service_event(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
    hids_active = false;
    assert(btkbd_command("bt forget"));
    btkbd_poll();
    assert(connect_calls == 1);
}

static void reboot_target_state(void) {
    target_store_pending = target_valid = connected = connecting = false;
    memset(target_addr, 0, sizeof(target_addr));
    open_bond_store();
    load_target();
}

static void test_power_cut_after_le_pairing_keeps_reconnect_target(void) {
    populate_bond_store();
    test_tlv->delete_tag(&flash_context, TARGET_TAG);
    /* Model the persisted SDK bond, followed by the identity callback. */
    test_identity_event_updates_reconnect_address();
    assert(target_store_pending);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    btkbd_poll(); /* Before the deliberate one-second delay expires. */
    reboot_target_state();
    assert(le_device_db_count() == 1); /* Bond survived physical power loss. */
    assert(target_valid); /* Application must still know whom to reconnect. */
}

static void test_identity_persists_then_rpa_b_reconnect_uses_identity(void) {
    populate_bond_store();
    test_identity_event_updates_reconnect_address();
    validation_now += 1000;
    btkbd_poll();
    reboot_target_state();
    assert(target_valid && bonded());
    start_le_connect();
    assert(accept_type == BD_ADDR_TYPE_LE_PUBLIC);
    assert(memcmp(accept_address, bond_address, 6) == 0);
    le_handle = 0x41;
    /* RPA B differs from both the saved identity and original RPA A. */
    uint8_t p[20] = {SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED, 18, 0x41, 0,
        BD_ADDR_TYPE_LE_RANDOM, 0x66, 0x55, 0x44, 0x33, 0x22, 0x42,
        BD_ADDR_TYPE_LE_PUBLIC, 6, 5, 4, 3, 2, 1, 0, 0};
    sm_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(memcmp(target_addr, bond_address, 6) == 0 && bonded());
}

static void test_forget_cancels_pending_store_and_reloads_resolver(void) {
    populate_bond_store();
    schedule_target_store();
    drop_target_bond();
    clear_target();
    validation_now += 2000;
    btkbd_poll();
    reboot_target_state();
    assert(!target_valid && le_device_db_count() == 0 && resolving_reloads == 1);
}

static void test_late_identity_event_cannot_undo_forget(void) {
    populate_bond_store();
    initialized = true;
    le_handle = 0x40;
    assert(btkbd_command("bt forget"));
    /* HCI disconnection is asynchronous; a pending SM identity event can
     * arrive after the command but before DISCONNECTION_COMPLETE. */
    uint8_t p[20] = {SM_EVENT_IDENTITY_CREATED, 18, 0x40, 0,
        BD_ADDR_TYPE_LE_RANDOM, 0x66, 0x55, 0x44, 0x33, 0x22, 0x41,
        BD_ADDR_TYPE_LE_PUBLIC, 6, 5, 4, 3, 2, 1, 0, 0};
    sm_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(!target_valid && !target_store_pending);
}

static void test_classic_hid_payload_still_decodes(void) {
    uint8_t br[9] = {0xa1, 2, 0, 4};
    handle_br_report(br, sizeof(br));
    check_shift_a();
}

static void test_classic_raw_payload_still_decodes(void) {
    handle_br_report(shift_a, sizeof(shift_a));
    check_shift_a();
}

static void classic_opened(void) {
    uint8_t p[17] = {HCI_EVENT_HID_META, 15, HID_SUBEVENT_CONNECTION_OPENED, 1, 0, 0};
    reverse_bd_addr(bond_address, p + 6);
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(connected && target_transport == TRANSPORT_BR);
}

static void test_classic_full_callback_and_reconnect(void) {
    initialized = radio_on = working = true;
    classic_opened();
    uint8_t p[16] = {HCI_EVENT_HID_META, 14, HID_SUBEVENT_REPORT, 1, 0, 9, 0,
        0xa1, 2, 0, 4};
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    check_shift_a();
    uint8_t close[5] = {HCI_EVENT_HID_META, 3, HID_SUBEVENT_CONNECTION_CLOSED, 1, 0};
    packet_handler(HCI_EVENT_PACKET, 0, close, sizeof(close));
    assert(!connected);
    validation_now += 3000;
    btkbd_poll();
    assert(br_connects == 1 && connecting);
}

static void test_power_cut_after_classic_ready_keeps_target(void) {
    populate_bond_store();
    test_tlv->delete_tag(&flash_context, TARGET_TAG);
    classic_opened();
    btkbd_poll();
    reboot_target_state();
    link_key_t key;
    link_key_type_t type;
    bd_addr_t address;
    memcpy(address, bond_address, 6);
    assert(classic_db->get_link_key(address, key, &type));
    assert(target_valid && target_transport == TRANSPORT_BR);
}

static void test_report_fields_from_different_ids_are_rejected(void) {
    uint8_t map[sizeof(keyboard_map) + 2];
    memcpy(map, keyboard_map, 30);
    map[30] = 0x85; map[31] = 2;
    memcpy(map + 32, keyboard_map + 30, sizeof(keyboard_map) - 30);
    hogp_keyboard_map_t parsed;
    assert(!hogp_keyboard_map_parse(map, sizeof(map), &parsed));
}

static bool interrupt_bond_write;
static void interruptible_flash_write(void *ctx, int bank, uint32_t offset,
                                      const uint8_t *data, uint32_t size) {
    flash_write(ctx, bank, offset, data, size);
    /* Model a BTstack IRQ immediately after flash_safe_execute restores IRQs,
     * between the foreground TLV value write and its entry-header write. */
    if (interrupt_bond_write) {
        interrupt_bond_write = false;
        sm_key_t ltk;
        uint8_t random[8] = {2};
        memset(ltk, 0xa5, sizeof(ltk));
        le_device_db_encryption_set(0, 43, random, ltk, 16, 1, 1, 1);
    }
}
static const hal_flash_bank_t interruptible_flash_driver = {
    flash_size, flash_alignment, flash_erase, flash_read, interruptible_flash_write,
};

static void test_deferred_target_write_survives_bond_write_interrupt(void) {
    populate_bond_store();
    flash_context.hal_flash_bank_impl = &interruptible_flash_driver;
    schedule_target_store();
    validation_now += 1000;
    interrupt_bond_write = true;
    btkbd_poll();
    assert(!interrupt_bond_write);
    reboot_target_state();
    sm_key_t ltk = {0};
    assert(le_device_db_count() == 1);
    le_device_db_encryption_get(0, NULL, NULL, ltk, NULL, NULL, NULL, NULL);
    for (unsigned i = 0; i < sizeof(ltk); ++i) assert(ltk[i] == 0xa5);
    assert(target_valid);
}

#define SECOND(name) {#name, test_##name}
static const struct { const char *name; void (*run)(void); } second[] = {
    SECOND(real_boot_dispatch_after_service_success),
    SECOND(deferred_retry_completes_real_report_delivery),
    SECOND(boot_success_without_input_eventually_recovers),
    SECOND(missing_key_requests_pairing_then_success_starts_hid),
    SECOND(repair_failure_disconnects_without_hid),
    SECOND(smp_abort_during_reencryption_does_not_stall),
    SECOND(pairing_then_reencryption_success_starts_once),
    SECOND(security_success_during_deferred_retry_keeps_retry),
    SECOND(disconnect_cancels_retry_and_new_link_starts_hid),
    SECOND(retry_does_not_run_after_radio_off),
    SECOND(retry_does_not_run_after_forget),
    SECOND(power_cut_after_le_pairing_keeps_reconnect_target),
    SECOND(identity_persists_then_rpa_b_reconnect_uses_identity),
    SECOND(forget_cancels_pending_store_and_reloads_resolver),
    SECOND(late_identity_event_cannot_undo_forget),
    SECOND(classic_hid_payload_still_decodes),
    SECOND(classic_raw_payload_still_decodes),
    SECOND(classic_full_callback_and_reconnect),
    SECOND(power_cut_after_classic_ready_keeps_target),
    SECOND(report_fields_from_different_ids_are_rejected),
    SECOND(deferred_target_write_survives_bond_write_interrupt),
};

int main(int argc, char **argv) {
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(second) / sizeof(second[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) puts(second[i].name);
        else if (strcmp(argv[1], second[i].name) == 0) {
            second[i].run();
            return 0;
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
