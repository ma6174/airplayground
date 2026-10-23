/*
 * Binary property lists ("bplist00"), the encoding AirPlay uses for every
 * SETUP request body and for the receiver replies that matter.
 *
 * Encoding takes a nested tree of caller-owned descriptors: a SETUP body is
 * built from scalars in place, with no allocation. Decoding exposes objects by
 * index, so callers walk a reply with the dict/array accessors instead of
 * materialising a tree.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AP_PLIST_NONE UINT64_MAX

typedef enum {
    AP_PLIST_BOOL = 0,
    AP_PLIST_INT,
    AP_PLIST_REAL,
    AP_PLIST_STRING,
    AP_PLIST_DATA,
    AP_PLIST_ARRAY,
    AP_PLIST_DICT,
} ap_plist_type_t;

typedef struct ap_plist_value ap_plist_value_t;

typedef struct {
    const char *key;
    const ap_plist_value_t *value;
} ap_plist_entry_t;

struct ap_plist_value {
    ap_plist_type_t type;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct {
            const void *ptr; /* STRING / DATA payload */
            size_t len;
        } bytes;
        struct {
            const ap_plist_value_t *items;
            size_t count;
        } array;
        struct {
            const ap_plist_entry_t *entries;
            size_t count;
        } dict;
    } as;
};

#define AP_PLIST_STR_VAL(s) \
    ((ap_plist_value_t){ AP_PLIST_STRING, { .bytes = { (s), sizeof(s) - 1 } } })
#define AP_PLIST_DATA_VAL(p, n) ((ap_plist_value_t){ AP_PLIST_DATA, { .bytes = { (p), (n) } } })

/* Encodes root into out. Returns the encoded length, or 0 when the tree is too
 * large or out is too small. The encoder owns static scratch, so one encoding
 * may run at a time (the sender only ever builds requests from one task). */
size_t ap_plist_encode(const ap_plist_value_t *root, void *out, size_t cap);

/* Decoder. The reader borrows the buffer; keep it alive while reading. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t object_area_end; /* start of the offset table */
    unsigned offset_size;
    unsigned ref_size;
    uint64_t object_count;
    uint64_t top_object;
    uint64_t offset_table;
} ap_plist_reader_t;

bool ap_plist_decode(ap_plist_reader_t *reader, const void *data, size_t len);

/* Object accessors. Indices are opaque handles; AP_PLIST_NONE means the key or
 * index was absent or had the wrong type. */
uint64_t ap_plist_root(const ap_plist_reader_t *reader);
bool ap_plist_dict_get(const ap_plist_reader_t *reader, uint64_t dict, const char *key, uint64_t *out);
bool ap_plist_array_get(const ap_plist_reader_t *reader, uint64_t array, size_t index, uint64_t *out);
size_t ap_plist_array_count(const ap_plist_reader_t *reader, uint64_t object);
bool ap_plist_uint(const ap_plist_reader_t *reader, uint64_t object, uint64_t *out);
bool ap_plist_int(const ap_plist_reader_t *reader, uint64_t object, int64_t *out);
bool ap_plist_bool(const ap_plist_reader_t *reader, uint64_t object, bool *out);
bool ap_plist_dict_uint(const ap_plist_reader_t *reader, uint64_t dict, const char *key, uint64_t *out);
bool ap_plist_dict_int(const ap_plist_reader_t *reader, uint64_t dict, const char *key, int64_t *out);
bool ap_plist_dict_bool(const ap_plist_reader_t *reader, uint64_t dict, const char *key, bool *out);
