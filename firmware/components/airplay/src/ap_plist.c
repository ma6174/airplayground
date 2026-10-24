#include "ap_plist.h"

#include <string.h>

#define AP_PLIST_MAX_OBJECTS 64
/* A dictionary stores its keys and values as separate references, so the
 * widest one the sender builds - the legacy SETUP 1 identity block - needs
 * two slots per entry. */
#define AP_PLIST_MAX_CHILDREN 48

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint8_t type;
    bool boolean;
    int64_t integer; /* INT value, or REAL bit pattern */
    const uint8_t *bytes;
    size_t bytes_len;
    uint32_t children[AP_PLIST_MAX_CHILDREN];
    uint8_t child_count;
} slot_t;

typedef struct {
    slot_t slots[AP_PLIST_MAX_OBJECTS];
    uint32_t count;
    bool ok;
} encoder_t;

/* The object graph is at most a few dozen nodes, but the flattened table is
 * several kilobytes; keeping it off the caller's stack matters on the ESP32. */
static encoder_t s_encoder;

static void put_be(uint8_t *out, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        out[size - 1 - i] = (uint8_t)(value >> (8 * i));
    }
}

static uint64_t get_be(const uint8_t *in, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        value = (value << 8) | in[i];
    }
    return value;
}

static unsigned bytes_for(uint64_t value)
{
    if (value <= 0xffu) {
        return 1;
    }
    if (value <= 0xffffu) {
        return 2;
    }
    if (value <= 0xffffffffu) {
        return 4;
    }
    return 8;
}

static uint8_t int_marker_size(unsigned size)
{
    switch (size) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 2;
    default:
        return 3;
    }
}

static uint32_t int_object_len(uint64_t value)
{
    return 1u + bytes_for(value);
}

static void write_int_object(uint8_t **cursor, uint64_t value)
{
    unsigned size = bytes_for(value);
    *(*cursor)++ = (uint8_t)(0x10 | int_marker_size(size));
    put_be(*cursor, value, size);
    *cursor += size;
}

static void append_child(slot_t *slot, uint32_t index)
{
    if (slot->child_count >= AP_PLIST_MAX_CHILDREN) {
        return;
    }
    slot->children[slot->child_count++] = index;
}

static uint32_t container_header_len(uint32_t count)
{
    return count < 15 ? 1u : 1u + int_object_len(count);
}

static void write_container_header(uint8_t **cursor, uint8_t base, uint32_t count)
{
    if (count < 15) {
        *(*cursor)++ = (uint8_t)(base | count);
        return;
    }
    *(*cursor)++ = (uint8_t)(base | 0x0f);
    write_int_object(cursor, count);
}

static int intern_slot(encoder_t *enc)
{
    if (enc->count >= AP_PLIST_MAX_OBJECTS) {
        enc->ok = false;
        return -1;
    }
    int index = (int)enc->count++;
    memset(&enc->slots[index], 0, sizeof(enc->slots[index]));
    return index;
}

static int intern_string(encoder_t *enc, const char *text)
{
    int index = intern_slot(enc);
    if (index < 0) {
        return -1;
    }
    slot_t *slot = &enc->slots[index];
    slot->type = AP_PLIST_STRING;
    slot->bytes = (const uint8_t *)text;
    slot->bytes_len = strlen(text);
    return index;
}

static int intern_value(encoder_t *enc, const ap_plist_value_t *value)
{
    /* Reserve the slot before recursing: child interning appends more objects,
     * and the slot pointer would be invalidated by a growing table. */
    int index = intern_slot(enc);
    if (index < 0) {
        return -1;
    }
    slot_t *slot = &enc->slots[index];
    slot->type = (uint8_t)value->type;

    switch (value->type) {
    case AP_PLIST_BOOL:
        slot->boolean = value->as.boolean;
        break;
    case AP_PLIST_INT:
        slot->integer = value->as.integer;
        break;
    case AP_PLIST_REAL:
        memcpy(&slot->integer, &value->as.real, sizeof(uint64_t));
        break;
    case AP_PLIST_STRING:
    case AP_PLIST_DATA:
        slot->bytes = (const uint8_t *)value->as.bytes.ptr;
        slot->bytes_len = value->as.bytes.len;
        break;
    case AP_PLIST_ARRAY:
        if (value->as.array.count > AP_PLIST_MAX_CHILDREN) {
            enc->ok = false;
            return -1;
        }
        for (size_t i = 0; i < value->as.array.count; i++) {
            int child = intern_value(enc, &value->as.array.items[i]);
            if (child < 0) {
                return -1;
            }
            append_child(&enc->slots[index], (uint32_t)child);
        }
        break;
    case AP_PLIST_DICT:
        if (value->as.dict.count * 2 > AP_PLIST_MAX_CHILDREN) {
            enc->ok = false;
            return -1;
        }
        /* bplist stores every key reference before any value reference. */
        uint32_t key_indices[AP_PLIST_MAX_CHILDREN / 2];
        uint32_t value_indices[AP_PLIST_MAX_CHILDREN / 2];
        for (size_t i = 0; i < value->as.dict.count; i++) {
            const ap_plist_entry_t *entry = &value->as.dict.entries[i];
            int key_index = intern_string(enc, entry->key);
            int value_index = key_index < 0 ? -1 : intern_value(enc, entry->value);
            if (key_index < 0 || value_index < 0) {
                return -1;
            }
            key_indices[i] = (uint32_t)key_index;
            value_indices[i] = (uint32_t)value_index;
        }
        for (size_t i = 0; i < value->as.dict.count; i++) {
            append_child(&enc->slots[index], key_indices[i]);
        }
        for (size_t i = 0; i < value->as.dict.count; i++) {
            append_child(&enc->slots[index], value_indices[i]);
        }
        break;
    default:
        enc->ok = false;
        return -1;
    }
    return index;
}

static uint32_t slot_length(const slot_t *slot, unsigned ref_size)
{
    switch (slot->type) {
    case AP_PLIST_BOOL:
        return 1;
    case AP_PLIST_INT:
        return slot->integer < 0 ? 9 : int_object_len((uint64_t)slot->integer);
    case AP_PLIST_REAL:
        return 9;
    case AP_PLIST_STRING:
    case AP_PLIST_DATA: {
        uint32_t header = slot->bytes_len < 15 ? 1u : 1u + int_object_len(slot->bytes_len);
        return header + (uint32_t)slot->bytes_len;
    }
    case AP_PLIST_ARRAY:
        return container_header_len(slot->child_count) + slot->child_count * ref_size;
    case AP_PLIST_DICT:
        return container_header_len(slot->child_count / 2) + slot->child_count * ref_size;
    default:
        return 0;
    }
}

static void write_slot(const slot_t *slot, uint32_t ref_size, uint8_t **cursor)
{
    switch (slot->type) {
    case AP_PLIST_BOOL:
        *(*cursor)++ = slot->boolean ? 0x09 : 0x08;
        break;
    case AP_PLIST_INT:
        if (slot->integer < 0) {
            *(*cursor)++ = 0x13;
            put_be(*cursor, (uint64_t)slot->integer, 8);
            *cursor += 8;
        } else {
            write_int_object(cursor, (uint64_t)slot->integer);
        }
        break;
    case AP_PLIST_REAL:
        *(*cursor)++ = 0x23;
        put_be(*cursor, (uint64_t)slot->integer, 8);
        *cursor += 8;
        break;
    case AP_PLIST_STRING:
    case AP_PLIST_DATA: {
        uint8_t base = slot->type == AP_PLIST_STRING ? 0x50 : 0x40;
        if (slot->bytes_len < 15) {
            *(*cursor)++ = (uint8_t)(base | slot->bytes_len);
        } else {
            *(*cursor)++ = (uint8_t)(base | 0x0f);
            write_int_object(cursor, slot->bytes_len);
        }
        if (slot->bytes_len > 0) {
            memcpy(*cursor, slot->bytes, slot->bytes_len);
            *cursor += slot->bytes_len;
        }
        break;
    }
    case AP_PLIST_ARRAY:
        write_container_header(cursor, 0xa0, slot->child_count);
        for (uint32_t i = 0; i < slot->child_count; i++) {
            put_be(*cursor, slot->children[i], ref_size);
            *cursor += ref_size;
        }
        break;
    case AP_PLIST_DICT:
        write_container_header(cursor, 0xd0, slot->child_count / 2);
        for (uint32_t i = 0; i < slot->child_count; i++) {
            put_be(*cursor, slot->children[i], ref_size);
            *cursor += ref_size;
        }
        break;
    default:
        break;
    }
}

size_t ap_plist_encode(const ap_plist_value_t *root, void *out, size_t cap)
{
    if (root == NULL || out == NULL) {
        return 0;
    }

    encoder_t *enc = &s_encoder;
    memset(enc, 0, sizeof(*enc));
    enc->ok = true;

    if (intern_value(enc, root) < 0 || !enc->ok) {
        return 0;
    }

    unsigned ref_size = bytes_for(enc->count - 1);
    uint32_t offset = 8; /* past "bplist00" */
    for (uint32_t i = 0; i < enc->count; i++) {
        enc->slots[i].offset = offset;
        enc->slots[i].length = slot_length(&enc->slots[i], ref_size);
        offset += enc->slots[i].length;
    }

    uint32_t offset_table = offset;
    /* Offsets index bytes before the table, so sizing for the table's widest
     * possible form is always sufficient. */
    unsigned offset_size = bytes_for((uint64_t)offset_table + (uint64_t)enc->count * 8u);

    size_t total = (size_t)offset_table + (size_t)enc->count * offset_size + 32;
    if (total > cap) {
        return 0;
    }

    uint8_t *base = (uint8_t *)out;
    memcpy(base, "bplist00", 8);

    uint8_t *cursor = base + 8;
    for (uint32_t i = 0; i < enc->count; i++) {
        write_slot(&enc->slots[i], ref_size, &cursor);
    }

    for (uint32_t i = 0; i < enc->count; i++) {
        put_be(base + offset_table + (size_t)i * offset_size, enc->slots[i].offset, offset_size);
    }

    uint8_t *trailer = base + offset_table + (size_t)enc->count * offset_size;
    memset(trailer, 0, 32);
    trailer[6] = (uint8_t)offset_size;
    trailer[7] = (uint8_t)ref_size;
    put_be(trailer + 8, enc->count, 8);
    put_be(trailer + 16, 0, 8); /* top object */
    put_be(trailer + 24, offset_table, 8);

    return total;
}

/* ------------------------------------------------------------------ decode */

typedef struct {
    uint8_t marker;
    const uint8_t *payload;
    size_t payload_len;
    uint64_t count; /* container entries / string length */
} object_t;

static bool load_object(const ap_plist_reader_t *reader, uint64_t index, object_t *out)
{
    if (index >= reader->object_count) {
        return false;
    }
    uint64_t offset =
        get_be(reader->data + reader->offset_table + index * reader->offset_size, reader->offset_size);
    if (offset >= reader->object_area_end) {
        return false;
    }

    const uint8_t *cursor = reader->data + offset;
    const uint8_t *end = reader->data + reader->object_area_end;

    memset(out, 0, sizeof(*out));
    out->marker = *cursor++;
    uint8_t high = (uint8_t)(out->marker >> 4);
    uint8_t low = (uint8_t)(out->marker & 0x0f);

    if (out->marker == 0x08 || out->marker == 0x09) {
        out->count = out->marker == 0x09 ? 1 : 0;
        return true;
    }

    if (high == 0x1 || high == 0x2) {
        unsigned size = 1u << low;
        if ((size_t)(end - cursor) < size) {
            return false;
        }
        out->payload = cursor;
        out->payload_len = size;
        return true;
    }

    if (high == 0x4 || high == 0x5 || high == 0x6 || high == 0xa || high == 0xb || high == 0xc ||
        high == 0xd) {
        uint64_t count = low;
        if (low == 0x0f) {
            if (cursor >= end) {
                return false;
            }
            uint8_t marker = *cursor++;
            if ((marker >> 4) != 0x1) {
                return false;
            }
            unsigned size = 1u << (marker & 0x0f);
            if ((size_t)(end - cursor) < size) {
                return false;
            }
            count = get_be(cursor, size);
            cursor += size;
        }
        out->count = count;

        size_t payload_len;
        if (high == 0x6) {
            payload_len = (size_t)count * 2; /* UTF-16BE code units */
        } else if (high == 0x4 || high == 0x5) {
            payload_len = (size_t)count;
        } else if (high == 0xd) {
            payload_len = (size_t)count * 2 * reader->ref_size;
        } else {
            payload_len = (size_t)count * reader->ref_size;
        }
        if ((size_t)(end - cursor) < payload_len) {
            return false;
        }
        out->payload = cursor;
        out->payload_len = payload_len;
        return true;
    }

    return false;
}

bool ap_plist_decode(ap_plist_reader_t *reader, const void *data, size_t len)
{
    if (reader == NULL || data == NULL || len < 8 + 32) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (memcmp(bytes, "bplist00", 8) != 0) {
        return false;
    }

    const uint8_t *trailer = bytes + len - 32;
    unsigned offset_size = trailer[6];
    unsigned ref_size = trailer[7];
    if (offset_size == 0 || offset_size > 8 || ref_size == 0 || ref_size > 8) {
        return false;
    }

    uint64_t object_count = get_be(trailer + 8, 8);
    uint64_t top_object = get_be(trailer + 16, 8);
    uint64_t offset_table = get_be(trailer + 24, 8);
    if (object_count == 0 || top_object >= object_count) {
        return false;
    }
    if (offset_table < 8 || offset_table >= len ||
        offset_table + object_count * offset_size > len) {
        return false;
    }

    reader->data = bytes;
    reader->len = len;
    reader->object_area_end = (size_t)offset_table;
    reader->offset_size = offset_size;
    reader->ref_size = ref_size;
    reader->object_count = object_count;
    reader->top_object = top_object;
    reader->offset_table = offset_table;
    return true;
}

uint64_t ap_plist_root(const ap_plist_reader_t *reader)
{
    return reader != NULL ? reader->top_object : AP_PLIST_NONE;
}

static uint64_t ref_at(const ap_plist_reader_t *reader, const object_t *container, size_t index)
{
    return get_be(container->payload + index * reader->ref_size, reader->ref_size);
}

bool ap_plist_dict_get(const ap_plist_reader_t *reader, uint64_t dict, const char *key, uint64_t *out)
{
    object_t container;
    if (reader == NULL || key == NULL || !load_object(reader, dict, &container)) {
        return false;
    }
    if ((container.marker >> 4) != 0xd) {
        return false;
    }
    size_t key_len = strlen(key);

    for (uint64_t i = 0; i < container.count; i++) {
        object_t entry_key;
        if (!load_object(reader, ref_at(reader, &container, (size_t)i), &entry_key)) {
            continue;
        }
        if ((entry_key.marker >> 4) != 0x5 || entry_key.count != key_len) {
            continue;
        }
        if (memcmp(entry_key.payload, key, key_len) != 0) {
            continue;
        }
        if (out != NULL) {
            *out = ref_at(reader, &container, (size_t)(container.count + i));
        }
        return true;
    }
    return false;
}

size_t ap_plist_array_count(const ap_plist_reader_t *reader, uint64_t object)
{
    object_t container;
    if (reader == NULL || !load_object(reader, object, &container)) {
        return 0;
    }
    return (container.marker >> 4) == 0xa ? (size_t)container.count : 0;
}

bool ap_plist_array_get(const ap_plist_reader_t *reader, uint64_t array, size_t index, uint64_t *out)
{
    object_t container;
    if (reader == NULL || !load_object(reader, array, &container)) {
        return false;
    }
    if ((container.marker >> 4) != 0xa || index >= container.count) {
        return false;
    }
    *out = ref_at(reader, &container, index);
    return true;
}

bool ap_plist_uint(const ap_plist_reader_t *reader, uint64_t object, uint64_t *out)
{
    object_t value;
    if (reader == NULL || !load_object(reader, object, &value)) {
        return false;
    }
    if ((value.marker >> 4) != 0x1) {
        return false;
    }
    if (out != NULL) {
        *out = get_be(value.payload, (unsigned)value.payload_len);
    }
    return true;
}

bool ap_plist_int(const ap_plist_reader_t *reader, uint64_t object, int64_t *out)
{
    object_t value;
    if (reader == NULL || !load_object(reader, object, &value)) {
        return false;
    }
    if ((value.marker >> 4) != 0x1) {
        return false;
    }
    uint64_t raw = get_be(value.payload, (unsigned)value.payload_len);
    if (value.payload_len == 8) {
        if (out != NULL) {
            *out = (int64_t)raw;
        }
    } else if (out != NULL) {
        *out = (int64_t)raw;
    }
    return true;
}

bool ap_plist_bool(const ap_plist_reader_t *reader, uint64_t object, bool *out)
{
    object_t value;
    if (reader == NULL || !load_object(reader, object, &value)) {
        return false;
    }
    if (value.marker == 0x08 || value.marker == 0x09) {
        if (out != NULL) {
            *out = value.marker == 0x09;
        }
        return true;
    }
    if ((value.marker >> 4) == 0x1) {
        if (out != NULL) {
            *out = get_be(value.payload, (unsigned)value.payload_len) != 0;
        }
        return true;
    }
    return false;
}

bool ap_plist_dict_uint(const ap_plist_reader_t *reader, uint64_t dict, const char *key,
                        uint64_t *out)
{
    uint64_t value;
    if (!ap_plist_dict_get(reader, dict, key, &value)) {
        return false;
    }
    return ap_plist_uint(reader, value, out);
}

bool ap_plist_dict_int(const ap_plist_reader_t *reader, uint64_t dict, const char *key, int64_t *out)
{
    uint64_t value;
    if (!ap_plist_dict_get(reader, dict, key, &value)) {
        return false;
    }
    return ap_plist_int(reader, value, out);
}

bool ap_plist_dict_bool(const ap_plist_reader_t *reader, uint64_t dict, const char *key, bool *out)
{
    uint64_t value;
    if (!ap_plist_dict_get(reader, dict, key, &value)) {
        return false;
    }
    return ap_plist_bool(reader, value, out);
}
