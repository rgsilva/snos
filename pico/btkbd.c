#include "btkbd.h"
#include "hidkeys.h"
#include "hogp.h"
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
#define HOG_DESC_STORAGE 500
#define TARGET_TAG 0x534b4244u
#define TARGET_MAGIC 0x534e4b42u
#define TARGET_VERSION 2u

typedef enum { TRANSPORT_BR, TRANSPORT_LE } transport_t;
typedef enum { NAME_MISSING, NAME_REQUESTED, NAME_DONE } name_state_t;
typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    uint32_t cod;
    uint16_t clock_offset;
    uint8_t page_scan_repetition_mode;
    transport_t transport;
    bool keyboard;
    name_state_t name_state;
    char name[64];
} scan_entry_t;
typedef struct {
    uint32_t magic, version;
    bd_addr_t addr;
    uint8_t transport, addr_type;
    char name[64];
} target_store_t;

static btstack_packet_callback_registration_t hci_events, sm_events;
static uint8_t hid_desc[HID_DESC_STORAGE], hog_desc[HOG_DESC_STORAGE];
static scan_entry_t scan[MAX_SCAN];
static size_t scan_count;
static bool initialized, radio_on, working, scanning, scan_print_pending;
static bool connected, connecting, descriptor_available, target_valid;
static bd_addr_t target_addr;
static bd_addr_type_t target_addr_type;
static transport_t target_transport;
static char target_name[64];
static uint16_t hid_cid, hids_cid;
static hci_con_handle_t le_handle = HCI_CON_HANDLE_INVALID;
static bool le_report_mode;
static hogp_keyboard_map_t le_map;
static uint32_t reconnect_at;

static bool same_addr(const bd_addr_t a, const bd_addr_t b) { return bd_addr_cmp(a, b) == 0; }
static const char *addr_str(const bd_addr_t addr) { return bd_addr_to_str(addr); }
static const char *transport_name(transport_t t) { return t == TRANSPORT_LE ? "LE" : "BR"; }
static bool looks_keyboard(uint32_t cod) { return (cod & 0x1f00u) == 0x0500u && (cod & 0x40u); }

static void copy_text(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int scan_index(transport_t transport, const bd_addr_t addr, bd_addr_type_t addr_type) {
    for (size_t i = 0; i < scan_count; ++i) {
        if (scan[i].transport == transport && scan[i].addr_type == addr_type && same_addr(scan[i].addr, addr)) {
            return (int)i;
        }
    }
    return -1;
}

static int nth_scan_entry(unsigned n) {
    unsigned seen = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < scan_count; ++i) {
            if ((pass == 0) != scan[i].keyboard) continue;
            if (++seen == n) return (int)i;
        }
    }
    return -1;
}

static void store_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv || !target_valid) return;
    target_store_t rec = { TARGET_MAGIC, TARGET_VERSION, {0}, target_transport, target_addr_type, {0} };
    memcpy(rec.addr, target_addr, sizeof(rec.addr));
    copy_text(rec.name, sizeof(rec.name), target_name);
    tlv->store_tag(ctx, TARGET_TAG, (const uint8_t *)&rec, sizeof(rec));
}

static void load_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    target_store_t rec;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv || tlv->get_tag(ctx, TARGET_TAG, (uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec) ||
        rec.magic != TARGET_MAGIC || rec.version > TARGET_VERSION) return;
    memcpy(target_addr, rec.addr, sizeof(target_addr));
    if (rec.version == 1) {
        target_addr_type = BD_ADDR_TYPE_ACL;
        target_transport = TRANSPORT_BR;
    } else if (rec.transport <= TRANSPORT_LE) {
        target_addr_type = rec.addr_type;
        target_transport = (transport_t)rec.transport;
    } else return;
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

/* One slot is deliberate: replacing it also removes the old transport's key. */
static void drop_target_bond(void) {
    if (!target_valid) return;
    if (target_transport == TRANSPORT_BR) {
        gap_drop_link_key_for_bd_addr(target_addr);
        return;
    }
    gap_auto_connection_stop_all();
    gap_whitelist_clear();
    for (int i = le_device_db_count() - 1; i >= 0; --i) le_device_db_remove(i);
}

static bool bonded(void) {
    if (!target_valid) return false;
    if (target_transport == TRANSPORT_LE) {
        for (int i = 0; i < le_device_db_count(); ++i) {
            bd_addr_t addr;
            int type;
            sm_key_t irk;
            le_device_db_info(i, &type, addr, irk);
            if (type == target_addr_type && same_addr(addr, target_addr)) return true;
        }
        return false;
    }
    link_key_t key;
    link_key_type_t type;
    return gap_get_link_key_for_bd_addr(target_addr, key, &type);
}

static void connect_target(uint32_t delay_ms) {
    reconnect_at = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

static void start_le_connect(void) {
    /* The accept list and bonded IRK resolve Logitech private addresses. */
    gap_whitelist_clear();
    if (gap_whitelist_add(target_addr_type, target_addr) == ERROR_CODE_SUCCESS &&
        gap_connect_with_whitelist() == ERROR_CODE_SUCCESS) {
        connecting = true;
        log_printf("bt: reconnecting to %s [LE]\n", target_name[0] ? target_name : "keyboard");
    } else connect_target(5000);
}

static void try_connect_now(void) {
    if (!working || !radio_on || !target_valid || connected || connecting || scanning) return;
    if (target_transport == TRANSPORT_LE) {
        start_le_connect();
        return;
    }
    uint8_t status = hid_host_connect(target_addr, HID_PROTOCOL_MODE_BOOT, &hid_cid);
    if (status == ERROR_CODE_SUCCESS) {
        connecting = true;
        log_printf("bt: connecting to %s [BR]\n", target_name[0] ? target_name : "keyboard");
    } else {
        log_printf("bt: connect failed to start (0x%02x)\n", status);
        connect_target(5000);
    }
}

static void request_next_name(void) {
    for (size_t i = 0; i < scan_count; ++i) {
        if (scan[i].transport != TRANSPORT_BR || scan[i].name_state != NAME_MISSING) continue;
        scan[i].name_state = NAME_REQUESTED;
        gap_remote_name_request(scan[i].addr, scan[i].page_scan_repetition_mode,
                                scan[i].clock_offset | 0x8000u);
        return;
    }
    scanning = false;
    scan_print_pending = true;
}

static void print_scan_results(void) {
    puts("bt scan:");
    if (!scan_count) {
        puts("  (no Bluetooth devices found)");
        return;
    }
    unsigned index = 1;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < scan_count; ++i) {
            if ((pass == 0) != scan[i].keyboard) continue;
            printf("  %u  [%s]  %s  %s  %s\n", index++, transport_name(scan[i].transport),
                   scan[i].name[0] ? scan[i].name : "(unnamed)", addr_str(scan[i].addr),
                   scan[i].keyboard ? "keyboard" : "-");
        }
    }
}

static uint32_t passkey_for_addr(const bd_addr_t addr) {
    uint32_t value = to_ms_since_boot(get_absolute_time());
    for (size_t i = 0; i < 6; ++i) value = value * 33u + addr[i];
    return value % 1000000u;
}

static void handle_br_report(const uint8_t *report, uint16_t len) {
    if (len && report[0] == 0xa1) {
        ++report;
        --len;
    }
    if (len >= HIDKEYS_REPORT_LEN) kbd_hid_boot_report(report);
}

static void handle_le_report(uint8_t report_id, const uint8_t *report, uint16_t len) {
    uint8_t boot[HIDKEYS_REPORT_LEN];
    if (!le_report_mode) {
        if (len >= sizeof(boot)) kbd_hid_boot_report(report);
    } else if (hogp_keyboard_report_to_boot(&le_map, report_id, report, len, boot)) {
        kbd_hid_boot_report(boot);
    }
}

static void le_hids_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS && !le_report_mode) {
                le_report_mode = true;
                log_puts("bt: LE boot protocol unavailable; trying report protocol");
                hids_client_connect(le_handle, le_hids_event, HID_PROTOCOL_MODE_REPORT, &hids_cid);
                return;
            }
            if (status != ERROR_CODE_SUCCESS) {
                log_printf("bt: LE HID setup failed (0x%02x)\n", status);
                gap_disconnect(le_handle);
                return;
            }
            hids_cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            if (le_report_mode) {
                const uint8_t *map = hids_client_descriptor_storage_get_descriptor_data(hids_cid, 0);
                uint16_t map_len = hids_client_descriptor_storage_get_descriptor_len(hids_cid, 0);
                if (!hogp_keyboard_map_parse(map, map_len, &le_map)) {
                    log_puts("bt: LE HID report map has no keyboard input");
                    gap_disconnect(le_handle);
                    return;
                }
            }
            connected = true;
            connecting = false;
            store_target();
            log_printf("bt: keyboard ready [LE %s]\n", le_report_mode ? "report" : "boot");
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            if (connected) handle_le_report(gattservice_subevent_hid_report_get_report_id(packet),
                                             gattservice_subevent_hid_report_get_report(packet),
                                             gattservice_subevent_hid_report_get_report_len(packet));
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    bool start_hids = false;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            log_puts("bt: LE pairing Just Works accepted");
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            printf("bt: passkey %06" PRIu32 " - type this on the keyboard, then Enter\n",
                   sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                log_puts("bt: LE pairing complete");
                start_hids = true;
            } else {
                log_printf("bt: LE pairing failed (0x%02x)\n",
                           sm_event_pairing_complete_get_status(packet));
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            start_hids = true;
            break;
        default:
            break;
    }
    if (start_hids) {
        le_report_mode = false;
        memset(&le_map, 0, sizeof(le_map));
        if (hids_client_connect(le_handle, le_hids_event, HID_PROTOCOL_MODE_BOOT, &hids_cid) != ERROR_CODE_SUCCESS) {
            gap_disconnect(le_handle);
        }
    }
}

static void add_le_scan_result(uint8_t *packet) {
    bd_addr_t addr;
    bd_addr_type_t type = gap_event_advertising_report_get_address_type(packet);
    gap_event_advertising_report_get_address(packet, addr);
    int index = scan_index(TRANSPORT_LE, addr, type);
    if (index < 0) {
        if (scan_count >= MAX_SCAN) return;
        index = (int)scan_count++;
        memset(&scan[index], 0, sizeof(scan[index]));
        memcpy(scan[index].addr, addr, sizeof(addr));
        scan[index].addr_type = type;
        scan[index].transport = TRANSPORT_LE;
        scan[index].name_state = NAME_DONE;
    }
    hogp_advertisement_t ad = {0};
    hogp_advertisement_parse(gap_event_advertising_report_get_data(packet),
                             gap_event_advertising_report_get_data_length(packet), &ad);
    scan[index].keyboard |= ad.hid_service || ad.keyboard_appearance;
    if (ad.name[0] && (!scan[index].name[0] || ad.complete_name)) {
        copy_text(scan[index].name, sizeof(scan[index].name), ad.name);
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
            if (working) {
                load_target();
                log_puts("bt: radio ready");
                if (target_valid) connect_target(500);
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            if (scanning) add_le_scan_result(packet);
            break;
        case GAP_EVENT_INQUIRY_RESULT: {
            if (scan_count >= MAX_SCAN) break;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if (scan_index(TRANSPORT_BR, addr, BD_ADDR_TYPE_ACL) >= 0) break;
            scan_entry_t *entry = &scan[scan_count++];
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->addr, addr, sizeof(addr));
            entry->addr_type = BD_ADDR_TYPE_ACL;
            entry->transport = TRANSPORT_BR;
            entry->page_scan_repetition_mode = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
            entry->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
            entry->cod = gap_event_inquiry_result_get_class_of_device(packet);
            entry->keyboard = looks_keyboard(entry->cod);
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int len = gap_event_inquiry_result_get_name_len(packet);
                if (len > (int)sizeof(entry->name) - 1) len = sizeof(entry->name) - 1;
                memcpy(entry->name, gap_event_inquiry_result_get_name(packet), len);
                entry->name[len] = '\0';
                entry->name_state = NAME_DONE;
            } else entry->name_state = NAME_MISSING;
            break;
        }
        case GAP_EVENT_INQUIRY_COMPLETE:
            gap_stop_scan();
            request_next_name();
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            int index = scan_index(TRANSPORT_BR, addr, BD_ADDR_TYPE_ACL);
            if (index >= 0) {
                if (hci_event_remote_name_request_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                    copy_text(scan[index].name, sizeof(scan[index].name),
                              hci_event_remote_name_request_complete_get_remote_name(packet));
                }
                scan[index].name_state = NAME_DONE;
            }
            request_next_name();
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST: {
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            char pin_text[7];
            snprintf(pin_text, sizeof(pin_text), "%06" PRIu32, passkey_for_addr(addr));
            printf("bt: passkey %s - type this on the keyboard, then Enter\n", pin_text);
            gap_pin_code_response(addr, pin_text);
            break;
        }
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
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
            if (gap_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) store_target();
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE &&
                target_valid && target_transport == TRANSPORT_LE) {
                le_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                connecting = false;
                log_puts("bt: LE connected; pairing");
                sm_request_pairing(le_handle);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (target_transport == TRANSPORT_LE && le_handle != HCI_CON_HANDLE_INVALID) {
                le_handle = HCI_CON_HANDLE_INVALID;
                connected = connecting = false;
                log_puts("bt: LE keyboard disconnected");
                if (target_valid && radio_on) connect_target(3000);
            }
            break;
        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    hid_subevent_incoming_connection_get_address(packet, addr);
                    if (target_valid && target_transport == TRANSPORT_BR && same_addr(addr, target_addr)) {
                        hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet),
                                                   HID_PROTOCOL_MODE_BOOT);
                    } else hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
                    break;
                case HID_SUBEVENT_CONNECTION_OPENED:
                    connecting = false;
                    if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                        connected = false;
                        connect_target(5000);
                        break;
                    }
                    hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    connected = true;
                    hid_subevent_connection_opened_get_bd_addr(packet, target_addr);
                    target_valid = true;
                    target_transport = TRANSPORT_BR;
                    store_target();
                    log_printf("bt: keyboard connected [BR] (%s)\n", addr_str(target_addr));
                    break;
                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
                    descriptor_available = hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS;
                    if (descriptor_available) log_puts("bt: keyboard ready [BR]");
                    break;
                case HID_SUBEVENT_REPORT:
                    if (connected && target_transport == TRANSPORT_BR) {
                        handle_br_report(hid_subevent_report_get_report(packet),
                                         hid_subevent_report_get_report_len(packet));
                    }
                    break;
                case HID_SUBEVENT_CONNECTION_CLOSED:
                    connected = connecting = descriptor_available = false;
                    hid_cid = 0;
                    log_puts("bt: keyboard disconnected [BR]");
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
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    gatt_client_init();
    hids_client_init(hog_desc, sizeof(hog_desc));
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
    sm_events.callback = sm_packet_handler;
    sm_add_event_handler(&sm_events);
    initialized = radio_on = true;
    hci_power_control(HCI_POWER_ON);
}

void btkbd_poll(void) {
    if (scan_print_pending) {
        scan_print_pending = false;
        print_scan_results();
    }
    if (initialized && radio_on && target_valid && !connected && !connecting &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - reconnect_at) >= 0) try_connect_now();
}

static void print_status(void) {
    const char *state = !initialized ? "uninitialized" : radio_on ? (working ? "on" : "starting") : "off";
    printf("bt: radio=%s connected=%s scanning=%s\n", state, connected ? "yes" : "no", scanning ? "yes" : "no");
    if (target_valid) {
        printf("bt: keyboard=[%s] %s %s bonded=%s\n", transport_name(target_transport),
               target_name[0] ? target_name : "(unnamed)", addr_str(target_addr), bonded() ? "yes" : "no");
    } else puts("bt: keyboard=(none)");
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
    gap_set_scan_params(1, 0x30, 0x30, 0);
    gap_set_scan_duplicate_filter(false);
    gap_start_scan();
    int err = gap_inquiry_start(INQUIRY_10S);
    if (err) {
        gap_stop_scan();
        scanning = false;
        printf("bt: scan failed (0x%02x)\n", err);
    } else puts("bt: scanning [LE] and [BR] for about 10 seconds...");
}

static bool parse_target(const char *arg, scan_entry_t *entry) {
    char *end = NULL;
    unsigned long n = strtoul(arg, &end, 10);
    if (end && *end == '\0' && n > 0) {
        int index = nth_scan_entry((unsigned)n);
        if (index < 0) return false;
        *entry = scan[index];
        return true;
    }
    memset(entry, 0, sizeof(*entry));
    if (!sscanf_bd_addr(arg, entry->addr)) return false;
    entry->transport = TRANSPORT_BR;
    entry->addr_type = BD_ADDR_TYPE_ACL;
    return true;
}

static void pair_target(const char *arg) {
    if (!initialized || !radio_on || !working) {
        puts("bt: radio not ready");
        return;
    }
    scan_entry_t entry;
    if (!parse_target(arg, &entry)) {
        puts("bt: unknown scan index or address");
        return;
    }
    if (scanning) {
        gap_inquiry_stop();
        gap_stop_scan();
        scanning = false;
        scan_print_pending = false;
    }
    if (connected && target_transport == TRANSPORT_BR && hid_cid) hid_host_disconnect(hid_cid);
    if (connected && target_transport == TRANSPORT_LE && le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
    drop_target_bond();
    memcpy(target_addr, entry.addr, sizeof(target_addr));
    target_addr_type = entry.addr_type;
    target_transport = entry.transport;
    copy_text(target_name, sizeof(target_name), entry.name);
    target_valid = true;
    connected = connecting = false;
    reconnect_at = 0;
    try_connect_now();
    puts("bt: pairing/connecting");
}

static void forget(void) {
    if (!initialized) {
        puts("bt: radio not ready");
        return;
    }
    drop_target_bond();
    if (hid_cid) hid_host_disconnect(hid_cid);
    if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
    clear_target();
    connected = connecting = false;
    puts("bt: forgotten");
}

bool btkbd_command(const char *line) {
    if (strcmp(line, "bt") == 0) { print_status(); return true; }
    if (strcmp(line, "bt scan") == 0) { start_scan(); return true; }
    if (strncmp(line, "bt pair ", 8) == 0) { pair_target(line + 8); return true; }
    if (strcmp(line, "bt forget") == 0) { forget(); return true; }
    if (strcmp(line, "bt off") == 0) {
        if (initialized) hci_power_control(HCI_POWER_OFF);
        radio_on = working = connected = connecting = false;
        puts("bt: off");
        return true;
    }
    if (strcmp(line, "bt on") == 0) {
        if (!initialized) puts("bt: radio not ready");
        else { radio_on = true; hci_power_control(HCI_POWER_ON); puts("bt: on"); }
        return true;
    }
    return false;
}
