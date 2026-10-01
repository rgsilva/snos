#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

/* Include production code to exercise private callbacks without changing it. */
#include "../btkbd.c"
#include "btstack_tlv_flash_bank.h"
#include "ble/le_device_db_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"

void validation_notify(btstack_packet_handler_t callback, uint8_t report_id,
                       const uint8_t report[8]);

static unsigned reports_seen, connect_calls, disconnect_calls;
static bool hids_active;
static uint8_t received[8];
static bd_addr_t accept_address;
static bd_addr_type_t accept_type;
static const btstack_tlv_t *test_tlv;
static btstack_tlv_flash_bank_t flash_context;
static const btstack_link_key_db_t *classic_db;
static const uint8_t keyboard_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xc0,
};
static const uint8_t shift_a[8] = {2, 0, 4};

/* Only external effects are stubbed. SDK event accessors remain real. */
int log_puts(const char *text) { (void)text; return 0; }
int log_printf(const char *format, ...) { (void)format; return 0; }
void kbd_hid_boot_report(const uint8_t report[8]) {
    ++reports_seen;
    memcpy(received, report, 8);
}
uint8_t gap_disconnect(hci_con_handle_t handle) {
    (void)handle;
    ++disconnect_calls;
    return ERROR_CODE_SUCCESS;
}
uint8_t hids_client_connect(hci_con_handle_t handle, btstack_packet_handler_t callback,
                            hid_protocol_mode_t mode, uint16_t *cid) {
    (void)handle; (void)callback; (void)mode; (void)cid;
    ++connect_calls;
    /* hids_client.c:1453 rejects an existing client on the same handle. */
    if (hids_active) return ERROR_CODE_COMMAND_DISALLOWED;
    hids_active = true;
    return ERROR_CODE_SUCCESS;
}
const uint8_t *hids_client_descriptor_storage_get_descriptor_data(uint16_t cid,
                                                                  uint8_t index) {
    (void)cid; (void)index;
    return keyboard_map;
}
uint16_t hids_client_descriptor_storage_get_descriptor_len(uint16_t cid, uint8_t index) {
    (void)cid; (void)index;
    return sizeof(keyboard_map);
}
void btstack_tlv_get_instance(const btstack_tlv_t **tlv, void **context) {
    *tlv = test_tlv; *context = &flash_context;
}
void sm_just_works_confirm(hci_con_handle_t handle) { (void)handle; }
uint8_t gap_whitelist_clear(void) { return ERROR_CODE_SUCCESS; }
uint8_t gap_whitelist_add(bd_addr_type_t type, const bd_addr_t address) {
    accept_type = type;
    memcpy(accept_address, address, 6);
    return ERROR_CODE_SUCCESS;
}
uint8_t gap_connect_with_whitelist(void) { return ERROR_CODE_SUCCESS; }
uint8_t gap_auto_connection_stop_all(void) { return ERROR_CODE_SUCCESS; }
void gap_drop_link_key_for_bd_addr(bd_addr_t address) {
    classic_db->delete_link_key(address);
}

static void check_shift_a(void) {
    assert(reports_seen == 1);
    assert(memcmp(received, shift_a, 8) == 0);
    hidkeys_t keys;
    uint8_t output[6];
    hidkeys_init(&keys);
    assert(hidkeys_report(&keys, 0, received, output, sizeof(output)) == 1);
    assert(output[0] == 'A');
}

static void test_sdk_boot_notification_reaches_keyboard(void) {
    connected = true;
    validation_notify(le_hids_event, 1, shift_a);
    check_shift_a();
}

/* Normalize only packet_type to isolate the separate inserted-ID defect. */
static void normalized_packet_type(uint8_t type, uint16_t channel,
                                   uint8_t *packet, uint16_t size) {
    (void)type;
    le_hids_event(HCI_EVENT_PACKET, channel, packet, size);
}

static void test_boot_notification_skips_sdk_report_id(void) {
    connected = true;
    validation_notify(normalized_packet_type, 1, shift_a);
    check_shift_a();
}

static void test_report_notification_skips_sdk_report_id(void) {
    connected = le_report_mode = true;
    assert(hogp_keyboard_map_parse(keyboard_map, sizeof(keyboard_map), &le_map));
    validation_notify(normalized_packet_type, 1, shift_a);
    check_shift_a();
}

static void security_event(uint8_t type, uint8_t status) {
    uint8_t packet[14] = {0};
    packet[0] = type;
    packet[1] = sizeof(packet) - 2;
    little_endian_store_16(packet, 2, 0x40);
    packet[11] = status;
    le_handle = 0x40;
    sm_packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void test_failed_reencryption_does_not_access_hid(void) {
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_PIN_OR_KEY_MISSING);
    assert(connect_calls == 0);
}

static void test_reencryption_then_pairing_complete_starts_hid_once(void) {
    /* sm.c emits both when sm_request_pairing requested bonded encryption. */
    security_event(SM_EVENT_REENCRYPTION_COMPLETE, ERROR_CODE_SUCCESS);
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1);
    assert(disconnect_calls == 0);
}

static void test_pairing_failure_does_not_access_hid(void) {
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_AUTHENTICATION_FAILURE);
    assert(connect_calls == 0);
}

static void test_successful_pairing_starts_hid(void) {
    security_event(SM_EVENT_PAIRING_COMPLETE, ERROR_CODE_SUCCESS);
    assert(connect_calls == 1);
}

static void test_boot_error_does_not_retry_a_live_client(void) {
    /* SDK emits failure before hids_finalize_client; a nested connect fails. */
    hids_active = true;
    uint8_t packet[8] = {HCI_EVENT_GATTSERVICE_META, 6,
                        GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED, 1, 0,
                        ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE};
    le_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    hids_active = false;
    /* Retrying inside this callback is rejected by the actual SDK contract. */
    assert(connect_calls == 0);
}

static void test_pair_by_scanned_le_address_keeps_transport(void) {
    scan_count = 1;
    scan[0].transport = TRANSPORT_LE;
    scan[0].addr_type = BD_ADDR_TYPE_LE_RANDOM;
    assert(sscanf_bd_addr("41:22:33:44:55:66", scan[0].addr));
    scan_entry_t entry;
    assert(parse_target("41:22:33:44:55:66", &entry));
    assert(entry.transport == TRANSPORT_LE);
    assert(entry.addr_type == BD_ADDR_TYPE_LE_RANDOM);
}

static void test_scan_deduplicates_same_public_address_across_transports(void) {
    scan_count = 1;
    scan[0].transport = TRANSPORT_BR;
    scan[0].addr_type = BD_ADDR_TYPE_ACL;
    assert(sscanf_bd_addr("11:22:33:44:55:66", scan[0].addr));
    assert(scan_index(TRANSPORT_LE, scan[0].addr, BD_ADDR_TYPE_LE_PUBLIC) == 0);
}

static void test_identity_event_updates_reconnect_address(void) {
    assert(sscanf_bd_addr("41:22:33:44:55:66", target_addr));
    target_addr_type = BD_ADDR_TYPE_LE_RANDOM;
    target_transport = TRANSPORT_LE;
    target_valid = true;
    uint8_t event[20] = {SM_EVENT_IDENTITY_CREATED, 18, 0x40, 0,
                        BD_ADDR_TYPE_LE_RANDOM, 0x66, 0x55, 0x44, 0x33, 0x22, 0x41,
                        BD_ADDR_TYPE_LE_PUBLIC, 6, 5, 4, 3, 2, 1, 0, 0};
    sm_packet_handler(HCI_EVENT_PACKET, 0, event, sizeof(event));
    start_le_connect();
    const bd_addr_t identity = {1, 2, 3, 4, 5, 6};
    assert(accept_type == BD_ADDR_TYPE_LE_PUBLIC);
    assert(memcmp(accept_address, identity, 6) == 0);
}

static void test_complete_name_survives_short_scan_response(void) {
    hogp_advertisement_t ad = {0};
    const uint8_t complete[] = {5, 9, 'E', 'R', 'G', 'O'};
    const uint8_t short_name[] = {2, 8, 'E'};
    hogp_advertisement_parse(complete, sizeof(complete), &ad);
    hogp_advertisement_parse(short_name, sizeof(short_name), &ad);
    assert(strcmp(ad.name, "ERGO") == 0 && ad.complete_name);
}

static void test_generic_hid_appearance_is_keyboard_candidate(void) {
    hogp_advertisement_t ad = {0};
    const uint8_t data[] = {3, 0x19, 0xc0, 3};
    hogp_advertisement_parse(data, sizeof(data), &ad);
    assert(ad.keyboard_appearance);
}

static void test_name_is_bounded_and_terminated(void) {
    hogp_advertisement_t ad = {0};
    uint8_t data[256];
    memset(data, 'x', sizeof(data));
    data[0] = 255; data[1] = 9;
    hogp_advertisement_parse(data, sizeof(data), &ad);
    assert(strlen(ad.name) == sizeof(ad.name) - 1);
}

/* Place the final byte at a PROT_NONE page: reads past len fault immediately. */
static uint8_t *guarded_end(size_t length, void **allocation) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    assert(length <= page);
    *allocation = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(*allocation != MAP_FAILED);
    assert(mprotect((uint8_t *)*allocation + page, page, PROT_NONE) == 0);
    return (uint8_t *)*allocation + page - length;
}

static void test_malformed_ad_never_reads_past_length(void) {
    const uint8_t types[] = {2, 3, 8, 9, 0x19};
    for (size_t length = 0; length <= 32; ++length) {
        void *allocation;
        uint8_t *data = guarded_end(length, &allocation);
        for (size_t t = 0; t < sizeof(types); ++t) {
            for (unsigned field = 0; field <= 255; ++field) {
                memset(data, 0x12, length);
                if (length) data[0] = field;
                if (length > 1) data[1] = types[t];
                hogp_advertisement_t ad = {0};
                hogp_advertisement_parse(data, length, &ad);
            }
        }
        munmap(allocation, (size_t)sysconf(_SC_PAGESIZE) * 2);
    }
}

static void test_report_item_prefixes_never_read_past_length(void) {
    for (size_t length = 0; length <= 8; ++length) {
        void *allocation;
        uint8_t *data = guarded_end(length, &allocation);
        for (unsigned prefix = 0; prefix <= 255; ++prefix) {
            memset(data, 0xff, length);
            if (length) data[0] = prefix;
            hogp_keyboard_map_t map;
            hogp_keyboard_map_parse(data, length, &map);
        }
        munmap(allocation, (size_t)sysconf(_SC_PAGESIZE) * 2);
    }
}

static void test_short_reports_are_rejected_at_guard_page(void) {
    hogp_keyboard_map_t map;
    assert(hogp_keyboard_map_parse(keyboard_map, sizeof(keyboard_map), &map));
    for (size_t length = 0; length < 8; ++length) {
        void *allocation;
        uint8_t *data = guarded_end(length, &allocation);
        memset(data, 0, length);
        uint8_t boot[8];
        assert(!hogp_keyboard_report_to_boot(&map, 1, data, length, boot));
        munmap(allocation, (size_t)sysconf(_SC_PAGESIZE) * 2);
    }
}

static void test_wrong_report_id_is_rejected(void) {
    hogp_keyboard_map_t map;
    uint8_t boot[8];
    assert(hogp_keyboard_map_parse(keyboard_map, sizeof(keyboard_map), &map));
    assert(!hogp_keyboard_report_to_boot(&map, 2, shift_a, 8, boot));
}

static void test_truncated_item_after_keyboard_is_rejected(void) {
    uint8_t descriptor[sizeof(keyboard_map) + 1];
    memcpy(descriptor, keyboard_map, sizeof(keyboard_map));
    descriptor[sizeof(keyboard_map)] = 0x77; /* Four-byte Report Size, no value. */
    hogp_keyboard_map_t map;
    assert(!hogp_keyboard_map_parse(descriptor, sizeof(descriptor), &map));
}

static void test_unclosed_keyboard_collection_is_rejected(void) {
    hogp_keyboard_map_t map;
    assert(!hogp_keyboard_map_parse(keyboard_map, sizeof(keyboard_map) - 1, &map));
}

static void test_other_report_id_does_not_shift_keyboard_offsets(void) {
    const uint8_t consumer[] = {5, 0x0c, 9, 1, 0xa1, 1, 0x85, 2,
                                0x75, 8, 0x95, 1, 0x81, 2, 0xc0};
    uint8_t descriptor[sizeof(consumer) + sizeof(keyboard_map)];
    memcpy(descriptor, consumer, sizeof(consumer));
    memcpy(descriptor + sizeof(consumer), keyboard_map, sizeof(keyboard_map));
    hogp_keyboard_map_t map;
    uint8_t boot[8];
    assert(hogp_keyboard_map_parse(descriptor, sizeof(descriptor), &map));
    assert(hogp_keyboard_report_to_boot(&map, 1, shift_a, 8, boot));
    assert(memcmp(boot, shift_a, 8) == 0);
}

static void test_missing_modifiers_do_not_turn_key_usage_into_modifiers(void) {
    const uint8_t descriptor[] = {5, 1, 9, 6, 0xa1, 1, 5, 7, 0x75, 8,
                                  0x95, 6, 0x19, 0, 0x29, 0x65, 0x81, 0, 0xc0};
    const uint8_t keys[6] = {4};
    hogp_keyboard_map_t map;
    uint8_t boot[8];
    /* Either explicitly reject this layout or synthesize zero modifiers. */
    if (!hogp_keyboard_map_parse(descriptor, sizeof(descriptor), &map)) return;
    assert(hogp_keyboard_report_to_boot(&map, 0, keys, sizeof(keys), boot));
    assert(boot[0] == 0 && boot[2] == 4);
}

static void test_overflowing_report_bit_count_is_rejected(void) {
    const uint8_t overflow[] = {0x77, 0, 0, 0, 0x80, 0x95, 2, 0x81, 1};
    uint8_t descriptor[sizeof(keyboard_map) + sizeof(overflow)];
    memcpy(descriptor, overflow, sizeof(overflow));
    memcpy(descriptor + sizeof(overflow), keyboard_map, sizeof(keyboard_map));
    hogp_keyboard_map_t map;
    assert(!hogp_keyboard_map_parse(descriptor, sizeof(descriptor), &map));
}

static uint8_t flash_banks[2][4096];
static const bd_addr_t bond_address = {1, 2, 3, 4, 5, 6};
static uint32_t flash_size(void *context) { (void)context; return 4096; }
static uint32_t flash_alignment(void *context) { (void)context; return 1; }
static void flash_erase(void *context, int bank) {
    (void)context;
    assert(bank >= 0 && bank < 2);
    memset(flash_banks[bank], 0xff, 4096);
}
static void flash_read(void *context, int bank, uint32_t offset,
                       uint8_t *buffer, uint32_t size) {
    (void)context;
    assert(bank >= 0 && bank < 2 && offset <= 4096 && size <= 4096 - offset);
    memcpy(buffer, flash_banks[bank] + offset, size);
}
static void flash_write(void *context, int bank, uint32_t offset,
                        const uint8_t *data, uint32_t size) {
    (void)context;
    assert(bank >= 0 && bank < 2 && offset <= 4096 && size <= 4096 - offset);
    for (uint32_t i = 0; i < size; ++i) flash_banks[bank][offset + i] &= data[i];
}
static const hal_flash_bank_t flash_driver = {
    flash_size, flash_alignment, flash_erase, flash_read, flash_write,
};

static void open_bond_store(void) {
    memset(&flash_context, 0, sizeof(flash_context));
    test_tlv = btstack_tlv_flash_bank_init_instance(&flash_context, &flash_driver, NULL);
    le_device_db_tlv_configure(test_tlv, &flash_context);
    classic_db = btstack_link_key_db_tlv_get_instance(test_tlv, &flash_context);
}

static void populate_bond_store(void) {
    memset(flash_banks, 0xff, sizeof(flash_banks));
    open_bond_store();
    sm_key_t key;
    memset(key, 0x5a, sizeof(key));
    memcpy(target_addr, bond_address, 6);
    target_addr_type = BD_ADDR_TYPE_LE_PUBLIC;
    target_transport = TRANSPORT_LE;
    target_valid = true;
    classic_db->put_link_key(target_addr, key, COMBINATION_KEY);
    int index = le_device_db_add(BD_ADDR_TYPE_LE_PUBLIC, target_addr, key);
    assert(index == 0);
    uint8_t random[8] = {1};
    le_device_db_encryption_set(index, 42, random, key, 16, 1, 1, 1);
    assert(test_tlv->store_tag(&flash_context, 0x534d4952u, key, 16) == 0); /* SMIR */
    assert(test_tlv->store_tag(&flash_context, 0x534d4552u, key, 16) == 0); /* SMER */
    store_target();
}

static void verify_both_bonds(void) {
    sm_key_t key, irk;
    bd_addr_t address;
    int type, key_size, secure;
    assert(le_device_db_count() == 1);
    le_device_db_info(0, &type, address, irk);
    assert(type == BD_ADDR_TYPE_LE_PUBLIC && memcmp(address, bond_address, 6) == 0);
    for (unsigned i = 0; i < 16; ++i) assert(irk[i] == 0x5a);
    le_device_db_encryption_get(0, NULL, NULL, key, &key_size, NULL, NULL, &secure);
    assert(key_size == 16 && secure == 1);
    for (unsigned i = 0; i < 16; ++i) assert(key[i] == 0x5a);
    link_key_type_t classic_type;
    assert(classic_db->get_link_key(target_addr, key, &classic_type));
    for (unsigned i = 0; i < 16; ++i) assert(key[i] == 0x5a);
    target_valid = false;
    load_target();
    assert(target_valid && target_transport == TRANSPORT_LE);
}

static void test_classic_and_le_bonds_persist_through_bank_compaction(void) {
    populate_bond_store();
    /* Force multiple migrations of a 4 KiB active bank in the 8 KiB store. */
    for (unsigned i = 0; i < 200; ++i) store_target();
    open_bond_store();
    verify_both_bonds();
    int classic_size = 0;
    for (unsigned i = 0; i < NVM_NUM_LINK_KEYS; ++i) {
        int size = test_tlv->get_tag(&flash_context, 0x42544c00u + i, NULL, 0);
        if (size) classic_size = size;
    }
    printf("TLV record sizes: Classic=%d LE=%d target=%d; active write offset=%u\n",
           classic_size,
           test_tlv->get_tag(&flash_context, 0x42544400u, NULL, 0), /* BTD0 */
           test_tlv->get_tag(&flash_context, TARGET_TAG, NULL, 0), flash_context.write_offset);
}

static void test_forget_le_removes_persistent_le_bond(void) {
    populate_bond_store();
    drop_target_bond();
    open_bond_store();
    assert(le_device_db_count() == 0);
}

static void test_forget_classic_removes_persistent_classic_bond(void) {
    populate_bond_store();
    target_transport = TRANSPORT_BR;
    drop_target_bond();
    open_bond_store();
    link_key_t key;
    link_key_type_t type;
    assert(!classic_db->get_link_key(target_addr, key, &type));
}

#define SCENARIO(name) {#name, test_##name}
static const struct { const char *name; void (*run)(void); } scenarios[] = {
    SCENARIO(sdk_boot_notification_reaches_keyboard),
    SCENARIO(boot_notification_skips_sdk_report_id),
    SCENARIO(report_notification_skips_sdk_report_id),
    SCENARIO(failed_reencryption_does_not_access_hid),
    SCENARIO(reencryption_then_pairing_complete_starts_hid_once),
    SCENARIO(pairing_failure_does_not_access_hid),
    SCENARIO(successful_pairing_starts_hid),
    SCENARIO(boot_error_does_not_retry_a_live_client),
    SCENARIO(pair_by_scanned_le_address_keeps_transport),
    SCENARIO(scan_deduplicates_same_public_address_across_transports),
    SCENARIO(identity_event_updates_reconnect_address),
    SCENARIO(complete_name_survives_short_scan_response),
    SCENARIO(generic_hid_appearance_is_keyboard_candidate),
    SCENARIO(name_is_bounded_and_terminated),
    SCENARIO(malformed_ad_never_reads_past_length),
    SCENARIO(report_item_prefixes_never_read_past_length),
    SCENARIO(short_reports_are_rejected_at_guard_page),
    SCENARIO(wrong_report_id_is_rejected),
    SCENARIO(truncated_item_after_keyboard_is_rejected),
    SCENARIO(unclosed_keyboard_collection_is_rejected),
    SCENARIO(other_report_id_does_not_shift_keyboard_offsets),
    SCENARIO(missing_modifiers_do_not_turn_key_usage_into_modifiers),
    SCENARIO(overflowing_report_bit_count_is_rejected),
    SCENARIO(classic_and_le_bonds_persist_through_bank_compaction),
    SCENARIO(forget_le_removes_persistent_le_bond),
    SCENARIO(forget_classic_removes_persistent_classic_bond),
};

int main(int argc, char **argv) {
    assert(argc == 2);
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        if (strcmp(argv[1], "--list") == 0) puts(scenarios[i].name);
        else if (strcmp(argv[1], scenarios[i].name) == 0) {
            scenarios[i].run();
            return 0;
        }
    }
    return strcmp(argv[1], "--list") == 0 ? 0 : 2;
}
