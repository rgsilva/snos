#include "btkbd.h"
#include "hidkeys.h"
#include "hogp.h"
#include "kbd.h"
#include "log.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#ifdef PICO_ON_DEVICE
#include "pico/async_context.h"
#endif
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
#define LE_BOOT_INPUT_TIMEOUT_MS 5000u
#define LE_PAIRING_TIMEOUT_MS 5000u

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
static bool le_hids_started, le_report_retry, le_boot_waiting, le_pair_retry;
static bool le_prefer_report_mode;
static bool le_pairing_waiting, le_events_cancelled;
static hogp_keyboard_map_t le_map;
static uint32_t reconnect_at;
static bool target_store_pending;
static uint32_t target_store_at, le_boot_wait_at, le_pairing_wait_at;
static uint32_t target_store_generation;

static void le_hids_event(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                          uint16_t size);

/* The controller provides these when BTstack's full BLE stack is linked. */
extern uint8_t gap_load_resolving_list_from_le_device_db(void) __attribute__((weak));
extern void sm_request_pairing(hci_con_handle_t handle) __attribute__((weak));
/* Older host fixtures do not link the controller's cancellation entry point. */
extern uint8_t gap_connect_cancel(void) __attribute__((weak));

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

static bool same_scan_address(const scan_entry_t *entry, const bd_addr_t addr,
                              bd_addr_type_t addr_type) {
    if (!same_addr(entry->addr, addr)) return false;
    if (entry->addr_type == addr_type) return true;
    return (entry->addr_type == BD_ADDR_TYPE_ACL && addr_type == BD_ADDR_TYPE_LE_PUBLIC) ||
           (entry->addr_type == BD_ADDR_TYPE_LE_PUBLIC && addr_type == BD_ADDR_TYPE_ACL);
}

static int scan_index(transport_t transport, const bd_addr_t addr, bd_addr_type_t addr_type) {
    for (size_t i = 0; i < scan_count; ++i) {
        if (scan[i].transport == transport && scan[i].addr_type == addr_type &&
            same_addr(scan[i].addr, addr)) {
            return (int)i;
        }
        if (same_scan_address(&scan[i], addr, addr_type)) {
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

static bool store_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (!tlv || !target_valid) return false;
    target_store_t rec = {
        TARGET_MAGIC, TARGET_VERSION, {0}, target_transport, target_addr_type, {0}
    };
    memcpy(rec.addr, target_addr, sizeof(rec.addr));
    copy_text(rec.name, sizeof(rec.name), target_name);
    if (tlv->store_tag(ctx, TARGET_TAG, (const uint8_t *)&rec, sizeof(rec))) {
        log_puts("bt: could not save keyboard target");
        return false;
    }
    return true;
}

/* Callbacks only mark the record dirty; btkbd_poll performs the flash write. */
static void schedule_target_store(void) {
    ++target_store_generation;
    target_store_pending = true;
    target_store_at = to_ms_since_boot(get_absolute_time());
}

static bool store_target_locked(void) {
#ifdef PICO_ON_DEVICE
    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    bool stored = store_target();
    async_context_release_lock(context);
    return stored;
#else
    return store_target();
#endif
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

static void delete_target(void) {
    const btstack_tlv_t *tlv = NULL;
    void *ctx = NULL;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv) tlv->delete_tag(ctx, TARGET_TAG);
}

static void clear_target(void) {
#ifdef PICO_ON_DEVICE
    async_context_t *context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(context);
    delete_target();
    async_context_release_lock(context);
#else
    delete_target();
#endif
    target_store_pending = false;
    target_valid = false;
    target_name[0] = '\0';
    memset(target_addr, 0, sizeof(target_addr));
}

static void reset_le_session(void) {
    le_hids_started = false;
    le_report_retry = false;
    le_report_mode = false;
    le_boot_waiting = false;
    le_pair_retry = false;
    le_pairing_waiting = false;
    hids_cid = 0;
}

/* Do this before waiting for an asynchronous HCI disconnect event. */
static void cancel_deferred_actions(void) {
    target_store_pending = false;
    reset_le_session();
    le_prefer_report_mode = false;
    le_events_cancelled = true;
}

/* Locally retire a link before its asynchronous disconnect completion arrives. */
static void disconnect_current_target(void) {
    hci_con_handle_t old_le_handle = le_handle;
    uint16_t old_hid_cid = hid_cid;
    bool cancel_le_connect = target_transport == TRANSPORT_LE && connecting &&
                             old_le_handle == HCI_CON_HANDLE_INVALID;

    cancel_deferred_actions();
    le_handle = HCI_CON_HANDLE_INVALID;
    hid_cid = 0;
    connected = connecting = descriptor_available = false;

    if (target_transport == TRANSPORT_LE) {
        if (old_le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(old_le_handle);
        else if (cancel_le_connect && gap_connect_cancel != NULL) gap_connect_cancel();
    } else if (old_hid_cid) {
        hid_host_disconnect(old_hid_cid);
    }
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
    if (gap_load_resolving_list_from_le_device_db != NULL &&
        gap_load_resolving_list_from_le_device_db() != ERROR_CODE_SUCCESS) {
        log_puts("bt: could not reload LE resolving list");
    }
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
    /* hids_client prepends this ID and includes it in its reported length. */
    if (!len || report[0] != report_id) return;
    ++report;
    --len;
    if (!le_report_mode) {
        if (len >= sizeof(boot)) kbd_hid_boot_report(report);
    } else if (hogp_keyboard_report_to_boot(&le_map, report_id, report, len, boot)) {
        kbd_hid_boot_report(boot);
    }
}

static void start_le_hids(void) {
    if (le_hids_started || le_handle == HCI_CON_HANDLE_INVALID) return;
    le_hids_started = true;
    le_report_mode = le_prefer_report_mode;
    le_report_retry = false;
    memset(&le_map, 0, sizeof(le_map));
    hid_protocol_mode_t mode = le_report_mode ? HID_PROTOCOL_MODE_REPORT : HID_PROTOCOL_MODE_BOOT;
    uint8_t status = hids_client_connect(le_handle, le_hids_event, mode, &hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        le_hids_started = false;
        log_printf("bt: LE HID setup could not start (0x%02x)\n", status);
        gap_disconnect(le_handle);
    }
}

static void start_le_report_hids(void) {
    if (!le_report_retry || le_handle == HCI_CON_HANDLE_INVALID) return;
    le_report_retry = false;
    le_report_mode = true;
    uint8_t status = hids_client_connect(le_handle, le_hids_event, HID_PROTOCOL_MODE_REPORT,
                                         &hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        le_hids_started = false;
        log_printf("bt: LE report HID setup could not start (0x%02x)\n", status);
        gap_disconnect(le_handle);
    }
}

static void start_le_report_fallback(void) {
    if (le_handle == HCI_CON_HANDLE_INVALID || !le_hids_started) return;
    le_boot_waiting = false;
    le_hids_started = false;
    /* A successful client remains allocated, so reconnect before changing mode. */
    le_prefer_report_mode = true;
    log_puts("bt: LE boot input unavailable; reconnecting in report protocol");
    gap_disconnect(le_handle);
}

static void le_hids_event(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                          uint16_t size) {
    (void)channel;
    (void)packet_type;
    if (size < 3 || hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
    if (initialized && !radio_on) return;
    if (le_events_cancelled) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            if (hids_cid && cid != hids_cid) return;
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS && !le_report_mode) {
                /* hids_client finalizes its live client after this callback returns. */
                le_report_retry = true;
                log_puts("bt: LE boot protocol unavailable; trying report protocol");
                return;
            }
            if (status != ERROR_CODE_SUCCESS) {
                le_hids_started = false;
                log_printf("bt: LE HID setup failed (0x%02x)\n", status);
                gap_disconnect(le_handle);
                return;
            }
            hids_cid = cid;
            if (le_report_mode) {
                const uint8_t *map =
                    hids_client_descriptor_storage_get_descriptor_data(hids_cid, 0);
                uint16_t map_len = hids_client_descriptor_storage_get_descriptor_len(hids_cid, 0);
                if (!hogp_keyboard_map_parse(map, map_len, &le_map)) {
                    log_puts("bt: LE HID report map has no keyboard input");
                    gap_disconnect(le_handle);
                    return;
                }
                le_prefer_report_mode = false;
            } else {
                /* Boot mode is ready only after its keyboard input report arrives. */
                le_boot_waiting = true;
                le_boot_wait_at = to_ms_since_boot(get_absolute_time()) + LE_BOOT_INPUT_TIMEOUT_MS;
                return;
            }
            connected = true;
            connecting = false;
            schedule_target_store();
            log_printf("bt: keyboard ready [LE %s]\n", le_report_mode ? "report" : "boot");
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            if (hids_cid &&
                gattservice_subevent_hid_report_get_hids_cid(packet) != hids_cid) return;
            if (le_boot_waiting) {
                uint8_t report_id = gattservice_subevent_hid_report_get_report_id(packet);
                const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
                uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
                if (report_id != HID_BOOT_MODE_KEYBOARD_ID || len <= HIDKEYS_REPORT_LEN ||
                    report[0] != report_id) break;
                le_boot_waiting = false;
                connected = true;
                connecting = false;
                schedule_target_store();
                log_puts("bt: keyboard ready [LE boot]");
            }
            if (connected) {
                handle_le_report(gattservice_subevent_hid_report_get_report_id(packet),
                                 gattservice_subevent_hid_report_get_report(packet),
                                 gattservice_subevent_hid_report_get_report_len(packet));
            }
            break;
        default:
            break;
    }
}

static void request_le_pairing_later(void) {
    le_pair_retry = true;
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                              uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (initialized && !radio_on) return;
    if (le_events_cancelled) return;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            if (sm_event_just_works_request_get_handle(packet) != le_handle) break;
            le_pairing_waiting = false;
            sm_just_works_confirm(le_handle);
            log_puts("bt: LE pairing Just Works accepted");
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            if (sm_event_passkey_display_number_get_handle(packet) != le_handle) break;
            /* This is protocol progress, not an unanswered pairing request. */
            le_pairing_waiting = false;
            printf("bt: passkey %06" PRIu32 " - type this on the keyboard, then Enter\n",
                   sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_PAIRING_STARTED:
            if (sm_event_pairing_started_get_handle(packet) != le_handle) break;
            /* Let the Security Manager's own timeout govern an active exchange. */
            le_pairing_waiting = false;
            log_puts("bt: LE pairing started");
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_handle(packet) != le_handle) break;
            le_pair_retry = false;
            le_pairing_waiting = false;
            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                log_puts("bt: LE pairing complete");
                start_le_hids();
            } else {
                log_printf("bt: LE pairing failed (0x%02x)\n",
                           sm_event_pairing_complete_get_status(packet));
                gap_disconnect(le_handle);
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE: {
            if (sm_event_reencryption_complete_get_handle(packet) != le_handle) break;
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                le_pair_retry = false;
                le_pairing_waiting = false;
                start_le_hids();
            } else {
                log_printf("bt: LE re-encryption failed (0x%02x); pairing again\n", status);
                /* Missing-key failures have already entered the retryable SM state. */
                if (status == ERROR_CODE_PIN_OR_KEY_MISSING && sm_request_pairing != NULL) {
                    sm_request_pairing(le_handle);
                    le_pairing_waiting = true;
                    le_pairing_wait_at =
                        to_ms_since_boot(get_absolute_time()) + LE_PAIRING_TIMEOUT_MS;
                } else request_le_pairing_later();
            }
            break;
        }
        case SM_EVENT_IDENTITY_CREATED: {
            bd_addr_t peer_addr;
            sm_event_identity_created_get_address(packet, peer_addr);
            if (sm_event_identity_created_get_handle(packet) != le_handle &&
                (!target_valid || target_transport != TRANSPORT_LE ||
                 !same_addr(peer_addr, target_addr))) break;
            target_addr_type = sm_event_identity_created_get_identity_addr_type(packet);
            sm_event_identity_created_get_identity_address(packet, target_addr);
            target_valid = true;
            schedule_target_store();
            break;
        }
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            bd_addr_t peer_addr;
            sm_event_identity_resolving_succeeded_get_address(packet, peer_addr);
            if (sm_event_identity_resolving_succeeded_get_handle(packet) != le_handle &&
                (!target_valid || target_transport != TRANSPORT_LE ||
                 !same_addr(peer_addr, target_addr))) break;
            target_addr_type = sm_event_identity_resolving_succeeded_get_identity_addr_type(packet);
            sm_event_identity_resolving_succeeded_get_identity_address(packet, target_addr);
            target_valid = true;
            schedule_target_store();
            break;
        }
        default:
            break;
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
    if (initialized && !radio_on) return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (!radio_on) break;
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
            if (!le_events_cancelled &&
                gap_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                schedule_target_store();
            }
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) ==
                    GAP_SUBEVENT_LE_CONNECTION_COMPLETE &&
                radio_on && target_valid && target_transport == TRANSPORT_LE && connecting) {
                if (gap_subevent_le_connection_complete_get_status(packet) != ERROR_CODE_SUCCESS) {
                    connecting = false;
                    connect_target(5000);
                    break;
                }
                le_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                le_events_cancelled = false;
                connecting = false;
                connected = false;
                reset_le_session();
                log_puts("bt: LE connected; pairing");
                if (sm_request_pairing != NULL) sm_request_pairing(le_handle);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (target_transport == TRANSPORT_LE &&
                hci_event_disconnection_complete_get_connection_handle(packet) == le_handle) {
                le_handle = HCI_CON_HANDLE_INVALID;
                connected = connecting = false;
                reset_le_session();
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
                    schedule_target_store();
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
    if (target_store_pending &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - target_store_at) >= 0) {
        uint32_t generation = target_store_generation;
        if (store_target_locked()) {
            /* Releasing the device lock may run a callback that dirties this record. */
            if (target_store_generation == generation) target_store_pending = false;
        }
        else target_store_at = to_ms_since_boot(get_absolute_time()) + 1000;
    }
    if (le_boot_waiting &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - le_boot_wait_at) >= 0) {
        start_le_report_fallback();
    }
    if (le_report_retry) start_le_report_hids();
    if (le_pair_retry) {
        le_pair_retry = false;
        if (radio_on && le_handle != HCI_CON_HANDLE_INVALID && sm_request_pairing != NULL) {
            sm_request_pairing(le_handle);
            le_pairing_waiting = true;
            le_pairing_wait_at = to_ms_since_boot(get_absolute_time()) + LE_PAIRING_TIMEOUT_MS;
        } else if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
    }
    if (le_pairing_waiting &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - le_pairing_wait_at) >= 0) {
        le_pairing_waiting = false;
        if (le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(le_handle);
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
    /* A 25% active LE scan duty leaves time for WiFi and Classic inquiry. */
    gap_set_scan_params(1, 0x60, 0x18, 0);
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
    for (size_t i = 0; i < scan_count; ++i) {
        if (same_addr(scan[i].addr, entry->addr)) {
            *entry = scan[i];
            return true;
        }
    }
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
    disconnect_current_target();
    drop_target_bond();
    memcpy(target_addr, entry.addr, sizeof(target_addr));
    target_addr_type = entry.addr_type;
    target_transport = entry.transport;
    copy_text(target_name, sizeof(target_name), entry.name);
    target_valid = true;
    reconnect_at = 0;
    try_connect_now();
    puts("bt: pairing/connecting");
}

static void forget(void) {
    if (!initialized) {
        puts("bt: radio not ready");
        return;
    }
    disconnect_current_target();
    drop_target_bond();
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
        if (scanning) {
            gap_inquiry_stop();
            gap_stop_scan();
        }
        scanning = scan_print_pending = false;
        le_handle = HCI_CON_HANDLE_INVALID;
        cancel_deferred_actions();
        radio_on = working = connected = connecting = false;
        if (initialized) hci_power_control(HCI_POWER_OFF);
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
