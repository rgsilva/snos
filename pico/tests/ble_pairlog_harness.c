/* Which Bluetooth lines are replies and which are unsolicited. The firmware's
 * own btkbd.c runs; its radio calls are the first validator's spies. */
#define main first_validation_main
#include "ble_validation_harness.c"
#undef main

uint32_t pairlog_now = 1000;
uint8_t sm_request_pairing_calls;

/* Radio entry points the pairing scenarios reach and do not care about. */
void gap_set_scan_params(uint8_t a, uint16_t b, uint16_t c, uint8_t d) { (void)a; (void)b; (void)c; (void)d; }
void gap_set_scan_duplicate_filter(bool enabled) { (void)enabled; }
void gap_start_scan(void) {}
void gap_stop_scan(void) {}
bool gap_get_link_key_for_bd_addr(bd_addr_t addr, link_key_t key, link_key_type_t *type) {
    (void)addr; (void)key; (void)type;
    return false;
}
int gap_inquiry_start(uint8_t units) { (void)units; return 0; }
int gap_inquiry_stop(void) { return 0; }
int gap_remote_name_request(const bd_addr_t addr, uint8_t a, uint16_t b) {
    (void)addr; (void)a; (void)b;
    return 0;
}
int gap_pin_code_response(const bd_addr_t addr, const char *pin) { (void)addr; (void)pin; return 0; }
int gap_ssp_confirmation_response(const bd_addr_t addr) { (void)addr; return 0; }
int hci_power_control(HCI_POWER_MODE mode) { (void)mode; return 0; }
uint8_t hid_host_connect(bd_addr_t addr, hid_protocol_mode_t mode, uint16_t *cid) {
    (void)addr; (void)mode; (void)cid;
    return ERROR_CODE_SUCCESS;
}
uint8_t hid_host_accept_connection(uint16_t cid, hid_protocol_mode_t mode) {
    (void)cid; (void)mode;
    return ERROR_CODE_SUCCESS;
}
uint8_t hid_host_decline_connection(uint16_t cid) { (void)cid; return ERROR_CODE_SUCCESS; }
void hid_host_disconnect(uint16_t cid) { (void)cid; }

static void reset_output(void) { log_out[0] = reply_out[0] = '\0'; }

static void ready_radio(void) {
    initialized = radio_on = working = true;
    scan_count = 1;
    scan[0].transport = TRANSPORT_LE;
    scan[0].addr_type = BD_ADDR_TYPE_LE_RANDOM;
    snprintf(scan[0].name, sizeof(scan[0].name), "Keys");
    assert(sscanf_bd_addr("41:22:33:44:55:66", scan[0].addr));
}

static void le_event(uint8_t type, uint16_t handle) {
    uint8_t packet[16] = {0};
    packet[0] = type;
    packet[1] = sizeof(packet) - 2;
    little_endian_store_16(packet, 2, handle);
    
    sm_packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void connected_event(void) {
    uint8_t packet[20] = {HCI_EVENT_META_GAP, 18, GAP_SUBEVENT_LE_CONNECTION_COMPLETE, 0};
    little_endian_store_16(packet, 4, 0x40);
    packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void hids_connected(void) {
    uint8_t packet[8] = {HCI_EVENT_GATTSERVICE_META, 6, GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED,
                         1, 0, ERROR_CODE_SUCCESS};
    le_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void boot_key_report(void) {
    uint8_t packet[18] = {HCI_EVENT_GATTSERVICE_META, 16, GATTSERVICE_SUBEVENT_HID_REPORT,
                          1, 0, 0, HID_BOOT_MODE_KEYBOARD_ID, 9, 0, HID_BOOT_MODE_KEYBOARD_ID};
    le_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void passkey_event(void) {
    uint8_t packet[16] = {0};
    packet[0] = SM_EVENT_PASSKEY_DISPLAY_NUMBER;
    packet[1] = sizeof(packet) - 2;
    little_endian_store_16(packet, 2, 0x40);
    little_endian_store_32(packet, 12, 123456);
    sm_packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void run_pairing(void) {
    ready_radio();
    btkbd_command("bt pair 1");
    connected_event();
    le_event(SM_EVENT_PAIRING_STARTED, 0x40);
    passkey_event();
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    hids_connected();
    boot_key_report();
}

static bool has(const char *buffer, const char *text) { return strstr(buffer, text) != NULL; }

/* Every line, in either class, starts with the Bluetooth prefix. */
static void all_prefixed(const char *buffer) {
    for (const char *p = buffer; *p; p = strchr(p, '\n') + 1) assert(!strncmp(p, "[BT] ", 5));
}

/* After `bt pair`, every progress line up to "keyboard ready" is a reply. */
static void test_pairing_progress_prints_with_logging_off(void) {
    run_pairing();
    assert(!log_out[0]);
    assert(has(reply_out, "[BT] reconnecting to Keys [LE]\n"));
    assert(has(reply_out, "[BT] LE connected; pairing\n"));
    assert(has(reply_out, "[BT] LE pairing started\n"));
    assert(has(reply_out, "[BT] passkey 123456 - type this on the keyboard, then Enter\n"));
    assert(has(reply_out, "[BT] LE pairing complete\n"));
    assert(has(reply_out, "[BT] press a key on the keyboard now\n"));
    assert(has(reply_out, "[BT] keyboard ready [LE boot]\n"));
    assert(!pairing_window);
    all_prefixed(reply_out);
}

/* The same events with no `bt pair` behind them are unsolicited; a passkey
 * is always shown. */
static void test_background_reconnect_is_unsolicited(void) {
    ready_radio();
    target_valid = true;
    target_transport = TRANSPORT_LE;
    connecting = true;
    memcpy(target_addr, scan[0].addr, 6);
    connected_event();
    le_event(SM_EVENT_PAIRING_STARTED, 0x40);
    passkey_event();
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    hids_connected();
    boot_key_report();
    assert(has(log_out, "[BT] LE connected; pairing\n"));
    assert(has(log_out, "[BT] press a key on the keyboard now\n"));
    assert(has(log_out, "[BT] keyboard ready [LE boot]\n"));
    assert(!has(reply_out, "[BT] LE"));
    assert(!has(reply_out, "keyboard ready"));
    assert(has(reply_out, "[BT] passkey 123456"));
    all_prefixed(log_out);
    all_prefixed(reply_out);
}

static void test_pairing_window_ends_after_two_minutes(void) {
    ready_radio();
    btkbd_command("bt pair 1");
    assert(pairing_watched());
    pairlog_now += 119000;
    assert(pairing_watched());
    pairlog_now += 2000;
    reset_output();
    connected_event();
    assert(!pairing_watched());
    assert(has(log_out, "[BT] LE connected; pairing\n"));
    assert(!reply_out[0]);
}

static void test_window_ends_on_forget_and_bt_off(void) {
    ready_radio();
    btkbd_command("bt pair 1");
    btkbd_command("bt forget");
    assert(!pairing_window);
    btkbd_command("bt pair 1");
    btkbd_command("bt off");
    assert(!pairing_window);
}

static void test_report_fallback_tells_the_user_to_wake_the_keyboard(void) {
    ready_radio();
    btkbd_command("bt pair 1");
    connected_event();
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    reset_output();
    hids_connected();
    assert(has(reply_out, "[BT] press a key on the keyboard now\n"));
    pairlog_now += 6000;
    btkbd_poll();
    assert(!log_out[0]);
    assert(has(reply_out, "[BT] LE boot input unavailable; reconnecting in report protocol\n"));
    assert(has(reply_out, "[BT] waiting for the keyboard: press a key to wake it\n"));
}

static void test_command_answers_always_print(void) {
    ready_radio();
    btkbd_command("bt");
    btkbd_command("bt forget");
    assert(!log_out[0]);
    assert(has(reply_out, "[BT] radio=on"));
    assert(has(reply_out, "[BT] forgotten\n"));
    reset_output();
    btkbd_command("bt scan");
    ready_radio();
    scan[0].keyboard = true;
    scanning = false;
    print_scan_results();
    assert(has(reply_out, "[BT] scanning [LE] and [BR] for about 10 seconds...\n"));
    assert(has(reply_out, "[BT] scan:\n"));
    assert(has(reply_out, "[BT]   1  [LE]  Keys  "));
    btkbd_command("bt on");
    btkbd_command("bt off");
    assert(!log_out[0]);
    all_prefixed(reply_out);
}

#define PL(name) {#name, test_##name}
static const struct { const char *name; void (*run)(void); } pairlog_scenarios[] = {
    PL(pairing_progress_prints_with_logging_off),
    PL(background_reconnect_is_unsolicited),
    PL(pairing_window_ends_after_two_minutes),
    PL(window_ends_on_forget_and_bt_off),
    PL(report_fallback_tells_the_user_to_wake_the_keyboard),
    PL(command_answers_always_print),
};

int main(int argc, char **argv) {
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(pairlog_scenarios) / sizeof(pairlog_scenarios[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) puts(pairlog_scenarios[i].name);
        else if (strcmp(argv[1], pairlog_scenarios[i].name) == 0) {
            pairlog_scenarios[i].run();
            return 0;
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
