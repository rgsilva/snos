#include "../relay.h"
uint8_t sent_type, sent_payload[3];
bool send_ok = true;
bool relay_send(uint8_t type, const uint8_t *data, size_t len) {
    sent_type = type;
    if (len != 3) return false;
    for (size_t i = 0; i < len; ++i) sent_payload[i] = data[i];
    return send_ok;
}
