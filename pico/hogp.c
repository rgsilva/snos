#include "hogp.h"

#include <string.h>

#define AD_SHORT_NAME 0x08u
#define AD_COMPLETE_NAME 0x09u
#define AD_UUID16_MORE 0x02u
#define AD_UUID16_COMPLETE 0x03u
#define AD_APPEARANCE 0x19u
#define HID_UUID 0x1812u

static uint32_t read_unsigned(const uint8_t *data, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= (uint32_t)data[i] << (8u * i);
    return value;
}

static void copy_name(hogp_advertisement_t *out, const uint8_t *data, size_t len, bool complete) {
    if (!complete && out->complete_name) return;
    if (len >= sizeof(out->name)) len = sizeof(out->name) - 1;
    memcpy(out->name, data, len);
    out->name[len] = '\0';
    out->complete_name = complete;
}

void hogp_advertisement_parse(const uint8_t *data, size_t len, hogp_advertisement_t *out) {
    size_t pos = 0;
    while (pos < len) {
        size_t field_len = data[pos++];
        if (!field_len || field_len > len - pos) break;
        uint8_t type = data[pos++];
        const uint8_t *value = &data[pos];
        size_t value_len = field_len - 1;
        if (type == AD_SHORT_NAME || type == AD_COMPLETE_NAME) {
            copy_name(out, value, value_len, type == AD_COMPLETE_NAME);
        } else if (type == AD_UUID16_MORE || type == AD_UUID16_COMPLETE) {
            for (size_t i = 0; i + 1 < value_len; i += 2) {
                if ((uint16_t)read_unsigned(&value[i], 2) == HID_UUID) out->hid_service = true;
            }
        } else if (type == AD_APPEARANCE && value_len >= 2) {
            uint16_t appearance = (uint16_t)read_unsigned(value, 2);
            if (appearance == 0x03c0u || appearance == 0x03c1u) out->keyboard_appearance = true;
        }
        pos += value_len;
    }
}

static bool get_item(const uint8_t *data, size_t len, size_t *pos, uint8_t *type,
                     uint8_t *tag, uint32_t *value) {
    if (*pos >= len) return false;
    uint8_t prefix = data[(*pos)++];
    if (prefix == 0xfeu) {
        if (*pos + 2 > len) return false;
        size_t long_len = data[*pos];
        *pos += 2;
        if (long_len > len - *pos) return false;
        *pos += long_len;
        return get_item(data, len, pos, type, tag, value);
    }
    size_t size = prefix & 3u;
    if (size == 3) size = 4;
    if (size > len - *pos) return false;
    *type = (prefix >> 2) & 3u;
    *tag = prefix >> 4;
    *value = read_unsigned(&data[*pos], size);
    *pos += size;
    return true;
}

bool hogp_keyboard_map_parse(const uint8_t *data, size_t len, hogp_keyboard_map_t *out) {
    uint32_t usage_page = 0, usage_min = 0, usage_max = 0;
    uint32_t report_size = 0, report_count = 0, report_id = 0;
    uint16_t bit_pos = 0;
    unsigned collection_depth = 0;
    bool keyboard_collection = false;

    memset(out, 0, sizeof(*out));
    for (size_t pos = 0;;) {
        uint8_t type, tag;
        uint32_t value;
        if (!get_item(data, len, &pos, &type, &tag, &value)) break;
        if (type == 1) {
            if (tag == 0) usage_page = value;
            else if (tag == 7) report_size = value;
            else if (tag == 8) report_id = value;
            else if (tag == 9) report_count = value;
        } else if (type == 2) {
            if (tag == 0) usage_min = usage_max = value;
            else if (tag == 1) usage_min = value;
            else if (tag == 2) usage_max = value;
        } else if (type == 0 && tag == 10) {
            ++collection_depth;
            if (usage_page == 1 && usage_min == 6 && value == 1) keyboard_collection = true;
        } else if (type == 0 && tag == 12) {
            if (collection_depth) --collection_depth;
            if (!collection_depth) keyboard_collection = false;
        } else if (type == 0 && tag == 8) {
            bool input = (value & 1u) == 0;
            if (keyboard_collection && input && usage_page == 7 && report_size && report_count) {
                if (usage_min == 0xe0u && usage_max == 0xe7u && report_size == 1 && report_count >= 8) {
                    out->modifiers_offset = bit_pos;
                    out->report_id = (uint8_t)report_id;
                } else if ((value & 2u) == 0 && report_size == 8 && report_count >= 6) {
                    out->keys_offset = bit_pos;
                    out->key_count = report_count > 6 ? 6 : (uint8_t)report_count;
                }
            }
            if (report_size && report_count && report_size * report_count <= 0xffffu - bit_pos) {
                bit_pos += (uint16_t)(report_size * report_count);
            } else {
                return false;
            }
            usage_min = usage_max = 0;
        }
    }
    out->valid = out->key_count == 6;
    return out->valid;
}

static bool get_bits(const uint8_t *data, size_t len, uint16_t offset, unsigned count,
                     uint8_t *value) {
    if ((size_t)offset + count > len * 8 || count > 8) return false;
    uint8_t result = 0;
    for (unsigned i = 0; i < count; ++i) result |= ((data[(offset + i) / 8] >> ((offset + i) & 7)) & 1u) << i;
    *value = result;
    return true;
}

bool hogp_keyboard_report_to_boot(const hogp_keyboard_map_t *map, uint8_t report_id,
                                  const uint8_t *report, size_t len, uint8_t boot[8]) {
    if (!map->valid || map->report_id != report_id) return false;
    memset(boot, 0, 8);
    if (!get_bits(report, len, map->modifiers_offset, 8, &boot[0])) return false;
    for (unsigned i = 0; i < 6; ++i) {
        if (!get_bits(report, len, map->keys_offset + 8u * i, 8, &boot[2 + i])) return false;
    }
    return true;
}
