#include "btkbd.h"
#include "hidkeys.h"
#include "kbd.h"
#include "log.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCAN 16
#define INQUIRY_10S 8
#define HID_DESC_STORAGE 300
#define TARGET_TAG 0x534b4244u
#define TARGET_MAGIC 0x534e4b42u
#define TARGET_VERSION 1u

typedef enum { NAME_MISSING, NAME_REQUESTED, NAME_DONE } name_state_t;
typedef struct {
    bd_addr_t addr;
    uint32_t cod;
    uint16_t clock_offset;
    uint8_t page_scan_repetition_mode;
    name_state_t name_state;
    char name[64];
} scan_entry_t;
typedef struct {
    uint32_t magic, version;
    bd_addr_t addr;
    char name[64];
} target_store_t;

static btstack_packet_callback_registration_t hci_events;
static uint8_t hid_desc[HID_DESC_STORAGE];
static scan_entry_t scan[MAX_SCAN];
static size_t scan_count;
static bool initialized, radio_on, working, scanning, scan_print_pending;
static bool connected, connecting, descriptor_available;
static bool target_valid;
static bd_addr_t target_addr;
static char target_name[64];
static uint16_t hid_cid;
static uint32_t reconnect_at;

static bool same_addr(const bd_addr_t a, const bd_addr_t b) { return bd_addr_cmp(a, b) == 0; }
static const char *addr_str(const bd_addr_t addr) { return bd_addr_to_str(addr); }

static bool looks_keyboard(uint32_t cod) {
    return (cod & 0x1f00u) == 0x0500u && (cod & 0x40u);
}

static int scan_index_for_addr(const bd_addr_t addr) {
    for (size_t i = 0; i < scan_count; ++i) if (same_addr(scan[i].addr, addr)) return (int)i;
    return -1;
}

static int nth_scan_entry(unsigned n) {
    unsigned seen = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < scan_count; ++i) {
            bool kbd = looks_keyboard(scan[i].cod);
            if ((pass == 0) != kbd) continue;
            if (++seen == n) return (int)i;
        }
    }
    return -1;
}

static void store_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv) return;
    target_store_t rec = { TARGET_MAGIC, TARGET_VERSION, {0}, {0} };
    memcpy(rec.addr, target_addr, sizeof(rec.addr));
    strncpy(rec.name, target_name, sizeof(rec.name) - 1);
    tlv->store_tag(ctx, TARGET_TAG, (const uint8_t *)&rec, sizeof(rec));
}

static void load_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv) return;
    target_store_t rec;
    int n = tlv->get_tag(ctx, TARGET_TAG, (uint8_t *)&rec, sizeof(rec));
    if (n != (int)sizeof(rec) || rec.magic != TARGET_MAGIC || rec.version != TARGET_VERSION) return;
    memcpy(target_addr, rec.addr, sizeof(target_addr));
    memcpy(target_name, rec.name, sizeof(target_name));
    target_name[sizeof(target_name) - 1] = '\0';
    target_valid = true;
}

static void clear_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv) tlv->delete_tag(ctx, TARGET_TAG);
    target_valid = false;
    target_name[0] = '\0';
    memset(target_addr, 0, sizeof(target_addr));
}

static bool bonded(void) {
    if (!target_valid) return false;
    link_key_t key;
    link_key_type_t type;
    return gap_get_link_key_for_bd_addr(target_addr, key, &type);
}

static void request_next_name(void) {
    for (size_t i = 0; i < scan_count; ++i) {
        if (scan[i].name_state != NAME_MISSING) continue;
        scan[i].name_state = NAME_REQUESTED;
        gap_remote_name_request(scan[i].addr, scan[i].page_scan_repetition_mode, scan[i].clock_offset | 0x8000u);
        return;
    }
    scanning = false;
    scan_print_pending = true;
}

static void connect_target(uint32_t delay_ms) {
    reconnect_at = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

static void try_connect_now(void) {
    if (!working || !radio_on || !target_valid || connected || connecting || scanning) return;
    uint8_t status = hid_host_connect(target_addr, HID_PROTOCOL_MODE_BOOT, &hid_cid);
    if (status == ERROR_CODE_SUCCESS) {
        connecting = true;
        log_printf("bt: connecting to %s (%s)\n", target_name[0] ? target_name : "(unnamed)", addr_str(target_addr));
    } else {
        log_printf("bt: connect failed to start (0x%02x)\n", status);
        connect_target(5000);
    }
}

static void print_scan_results(void) {
    puts("bt scan:");
    if (!scan_count) {
        puts("  (no Classic devices found)");
        return;
    }
    unsigned index = 1;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < scan_count; ++i) {
            bool kbd = looks_keyboard(scan[i].cod);
            if ((pass == 0) != kbd) continue;
            printf("  %u  %s  %s  cod=0x%06" PRIx32 "  %s\n", index++,
                   scan[i].name[0] ? scan[i].name : "(unnamed)",
                   addr_str(scan[i].addr), scan[i].cod, kbd ? "keyboard" : "-");
        }
    }
}

static uint32_t passkey_for_addr(const bd_addr_t addr) {
    uint32_t v = to_ms_since_boot(get_absolute_time());
    for (size_t i = 0; i < 6; ++i) v = v * 33u + addr[i];
    return v % 1000000u;
}

static void handle_hid_report(const uint8_t *report, uint16_t len) {
    if (len && report[0] == 0xa1) {
        ++report;
        --len;
    }
    if (len < HIDKEYS_REPORT_LEN) return;
    kbd_hid_boot_report(report);
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t addr;
    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
            if (working) {
                load_target();
                log_puts("bt: radio ready");
                if (target_valid) connect_target(500);
            }
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            if (scan_count >= MAX_SCAN) break;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (scan_index_for_addr(addr) >= 0) break;
            memcpy(scan[scan_count].addr, addr, sizeof(addr));
            scan[scan_count].page_scan_repetition_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
            scan[scan_count].clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
            scan[scan_count].cod = gap_event_inquiry_result_get_class_of_device(packet);
            scan[scan_count].name[0] = '\0';
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int len = gap_event_inquiry_result_get_name_len(packet);
                if (len > (int)sizeof(scan[scan_count].name) - 1) len = sizeof(scan[scan_count].name) - 1;
                memcpy(scan[scan_count].name, gap_event_inquiry_result_get_name(packet), len);
                scan[scan_count].name[len] = '\0';
                scan[scan_count].name_state = NAME_DONE;
            } else {
                scan[scan_count].name_state = NAME_MISSING;
            }
            ++scan_count;
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            request_next_name();
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            int idx = scan_index_for_addr(addr);
            if (idx >= 0) {
                if (hci_event_remote_name_request_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                    strncpy(scan[idx].name, hci_event_remote_name_request_complete_get_remote_name(packet),
                            sizeof(scan[idx].name) - 1);
                    scan[idx].name[sizeof(scan[idx].name) - 1] = '\0';
                }
                scan[idx].name_state = NAME_DONE;
            }
            request_next_name();
            break;
        case HCI_EVENT_PIN_CODE_REQUEST: {
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            uint32_t pin = passkey_for_addr(addr);
            char pin_text[7];
            snprintf(pin_text, sizeof(pin_text), "%06" PRIu32, pin);
            printf("bt: passkey %s - type this on the keyboard, then Enter\n", pin_text);
            gap_pin_code_response(addr, pin_text);
            break;
        }
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            hci_event_user_passkey_notification_get_bd_addr(packet, addr);
            printf("bt: passkey %06" PRIu32 " - type this on the keyboard, then Enter\n",
                   hci_event_user_passkey_notification_get_numeric_value(packet));
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            gap_ssp_confirmation_response(addr);
            log_puts("bt: pairing confirmation accepted");
            break;
        case GAP_EVENT_PAIRING_STARTED:
            log_puts("bt: pairing started");
            break;
        case GAP_EVENT_PAIRING_COMPLETE:
            if (gap_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                log_puts("bt: pairing complete");
                store_target();
            } else {
                log_printf("bt: pairing failed (0x%02x)\n", gap_event_pairing_complete_get_status(packet));
            }
            break;
        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    hid_subevent_incoming_connection_get_address(packet, addr);
                    if (target_valid && same_addr(addr, target_addr)) {
                        hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet),
                                                   HID_PROTOCOL_MODE_BOOT);
                    } else {
                        hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
                    }
                    break;
                case HID_SUBEVENT_CONNECTION_OPENED:
                    connecting = false;
                    if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                        connected = false;
                        descriptor_available = false;
                        hid_cid = 0;
                        log_printf("bt: connection failed (0x%02x)\n",
                                   hid_subevent_connection_opened_get_status(packet));
                        connect_target(5000);
                        break;
                    }
                    hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    hid_subevent_connection_opened_get_bd_addr(packet, addr);
                    memcpy(target_addr, addr, sizeof(target_addr));
                    target_valid = true;
                    connected = true;
                    descriptor_available = false;
                    store_target();
                    log_printf("bt: keyboard connected (%s)\n", addr_str(target_addr));
                    break;
                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
                    descriptor_available = hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS;
                    if (descriptor_available) log_puts("bt: keyboard ready");
                    else log_printf("bt: HID descriptor unavailable (0x%02x)\n",
                                    hid_subevent_descriptor_available_get_status(packet));
                    break;
                case HID_SUBEVENT_REPORT:
                    if (connected) handle_hid_report(hid_subevent_report_get_report(packet),
                                                     hid_subevent_report_get_report_len(packet));
                    break;
                case HID_SUBEVENT_CONNECTION_CLOSED:
                    connected = false;
                    connecting = false;
                    descriptor_available = false;
                    hid_cid = 0;
                    log_puts("bt: keyboard disconnected");
                    if (target_valid && radio_on) connect_target(3000);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

void btkbd_init(void) {
    if (initialized) return;
    if (!btstack_cyw43_init(cyw43_arch_async_context())) {
        log_puts("bt: init failed");
        return;
    }
    l2cap_init();
    hid_host_init(hid_desc, sizeof(hid_desc));
    hid_host_register_packet_handler(packet_handler);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    gap_connectable_control(1);
    gap_discoverable_control(1);
    hci_events.callback = packet_handler;
    hci_add_event_handler(&hci_events);
    initialized = true;
    radio_on = true;
    hci_power_control(HCI_POWER_ON);
}

void btkbd_poll(void) {
    if (scan_print_pending) {
        scan_print_pending = false;
        print_scan_results();
    }
    if (initialized && radio_on && target_valid && !connected && !connecting &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - reconnect_at) >= 0) {
        try_connect_now();
    }
}

static void print_status(void) {
    const char *state = "uninitialized";
    if (initialized) state = radio_on ? (working ? "on" : "starting") : "off";
    printf("bt: radio=%s connected=%s scanning=%s\n", state, connected ? "yes" : "no",
           scanning ? "yes" : "no");
    if (target_valid) {
        printf("bt: keyboard=%s %s bonded=%s\n", target_name[0] ? target_name : "(unnamed)",
               addr_str(target_addr), bonded() ? "yes" : "no");
    } else {
        puts("bt: keyboard=(none)");
    }
}

static void start_scan(void) {
    if (!initialized || !radio_on || !working) {
        puts("bt: radio not ready");
        return;
    }
    if (scanning) {
        puts("bt: scan already running");
        return;
    }
    scan_count = 0;
    memset(scan, 0, sizeof(scan));
    scanning = true;
    scan_print_pending = false;
    int err = gap_inquiry_start(INQUIRY_10S);
    if (err) {
        scanning = false;
        printf("bt: scan failed (0x%02x)\n", err);
    } else {
        puts("bt: scanning for about 10 seconds...");
    }
}

static bool parse_addr_or_index(const char *arg, bd_addr_t out, char *name, size_t name_size) {
    char *end = NULL;
    unsigned long n = strtoul(arg, &end, 10);
    if (end && *end == '\0' && n > 0) {
        int idx = nth_scan_entry((unsigned)n);
        if (idx < 0) return false;
        memcpy(out, scan[idx].addr, sizeof(bd_addr_t));
        strncpy(name, scan[idx].name, name_size - 1);
        name[name_size - 1] = '\0';
        return true;
    }
    if (sscanf_bd_addr(arg, out)) {
        name[0] = '\0';
        return true;
    }
    return false;
}

static void pair_target(const char *arg) {
    if (!initialized || !radio_on || !working) {
        puts("bt: radio not ready");
        return;
    }
    bd_addr_t addr;
    char name[64];
    if (!parse_addr_or_index(arg, addr, name, sizeof(name))) {
        puts("bt: unknown scan index or address");
        return;
    }
    if (scanning) {
        gap_inquiry_stop();
        scanning = false;
        scan_print_pending = false;
    }
    if (connected && hid_cid) hid_host_disconnect(hid_cid);
    memcpy(target_addr, addr, sizeof(target_addr));
    strncpy(target_name, name, sizeof(target_name) - 1);
    target_name[sizeof(target_name) - 1] = '\0';
    target_valid = true;
    gap_drop_link_key_for_bd_addr(target_addr);
    store_target();
    connecting = false;
    connected = false;
    reconnect_at = 0;
    try_connect_now();
    puts("bt: pairing/connecting");
}

static void forget(void) {
    if (!initialized) {
        puts("bt: radio not ready");
        return;
    }
    if (connected && hid_cid) hid_host_disconnect(hid_cid);
    if (target_valid) gap_drop_link_key_for_bd_addr(target_addr);
    clear_target();
    connected = false;
    connecting = false;
    puts("bt: forgotten");
}

bool btkbd_command(const char *line) {
    if (strcmp(line, "bt") == 0) {
        print_status();
        return true;
    }
    if (strcmp(line, "bt scan") == 0) {
        start_scan();
        return true;
    }
    if (strncmp(line, "bt pair ", 8) == 0) {
        pair_target(line + 8);
        return true;
    }
    if (strcmp(line, "bt forget") == 0) {
        forget();
        return true;
    }
    if (strcmp(line, "bt off") == 0) {
        if (initialized) hci_power_control(HCI_POWER_OFF);
        radio_on = false;
        working = false;
        connected = false;
        connecting = false;
        puts("bt: off");
        return true;
    }
    if (strcmp(line, "bt on") == 0) {
        if (!initialized) {
            puts("bt: radio not ready");
        } else {
            radio_on = true;
            hci_power_control(HCI_POWER_ON);
            puts("bt: on");
        }
        return true;
    }
    return false;
}
