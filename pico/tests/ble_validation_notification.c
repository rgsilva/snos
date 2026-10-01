/* Exercise the installed SDK's notification builder and callback contract. */
#define hids_client_connect sdk_hids_client_connect
#define hids_client_descriptor_storage_get_descriptor_data sdk_descriptor_data
#define hids_client_descriptor_storage_get_descriptor_len sdk_descriptor_len
#include "ble/gatt-service/hids_client.c"

void validation_notify(btstack_packet_handler_t callback, uint8_t report_id,
                       const uint8_t report[8]) {
    hids_client_t client;
    memset(&client, 0, sizeof(client));
    client.con_handle = 0x40;
    client.cid = 1;
    client.client_handler = callback;
    client.num_reports = 1;
    client.reports[0].value_handle = 0x30;
    client.reports[0].report_id = report_id;
    clients = NULL;
    btstack_linked_list_add(&clients, (btstack_linked_item_t *)&client);

    uint8_t packet[20] = {GATT_EVENT_NOTIFICATION, 18};
    little_endian_store_16(packet, 2, 0x40);
    little_endian_store_16(packet, 8, 0x30);
    little_endian_store_16(packet, 10, 8);
    memcpy(packet + 12, report, 8);
    handle_notification_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    clients = NULL;
}
