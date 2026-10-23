/* Host tests for the binary plist codec and the SETUP bodies.
 *
 * The encoder output is verified twice: once by this program (round-trip
 * through the decoder) and once by check.py, which reads the same files with
 * Python's plistlib. The decoder is fed files produced by plistlib, so neither
 * direction can be self-consistent-but-wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap_plist.h"
#include "ap_setup.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void write_file(const char *directory, const char *name, const void *data, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *handle = fopen(path, "wb");
    if (handle == NULL) {
        perror(path);
        failures++;
        return;
    }
    if (fwrite(data, 1, len, handle) != len) {
        failures++;
    }
    fclose(handle);
    printf("wrote %s (%u bytes)\n", path, (unsigned)len);
}

static size_t read_file(const char *directory, const char *name, uint8_t *buffer, size_t capacity)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *handle = fopen(path, "rb");
    if (handle == NULL) {
        perror(path);
        failures++;
        return 0;
    }
    size_t len = fread(buffer, 1, capacity, handle);
    fclose(handle);
    return len;
}

static void test_encode_all_types(const char *directory)
{
    printf("test_encode_all_types\n");

    const char *string_value_bytes = "hello";
    const uint8_t blob[] = { 0x00, 0x01, 0x02, 0xff };

    ap_plist_value_t string_value = { .type = AP_PLIST_STRING,
                                      .as.bytes = { string_value_bytes, 5 } };
    ap_plist_value_t empty_value = { .type = AP_PLIST_STRING, .as.bytes = { "", 0 } };
    ap_plist_value_t small_value = { .type = AP_PLIST_INT, .as.integer = 42 };
    ap_plist_value_t big_value = { .type = AP_PLIST_INT, .as.integer = 1234567890123LL };
    ap_plist_value_t negative_value = { .type = AP_PLIST_INT, .as.integer = -7 };
    ap_plist_value_t true_value = { .type = AP_PLIST_BOOL, .as.boolean = true };
    ap_plist_value_t false_value = { .type = AP_PLIST_BOOL, .as.boolean = false };
    ap_plist_value_t real_value = { .type = AP_PLIST_REAL, .as.real = 1.5 };
    ap_plist_value_t data_value = { .type = AP_PLIST_DATA, .as.bytes = { blob, sizeof(blob) } };

    ap_plist_value_t nested_integer = { .type = AP_PLIST_INT, .as.integer = 3 };
    ap_plist_entry_t nested_entry = { "nested", &nested_integer };
    ap_plist_value_t nested_dict = { .type = AP_PLIST_DICT,
                                     .as.dict = { &nested_entry, 1 } };

    ap_plist_value_t list_items[4] = {
        { .type = AP_PLIST_INT, .as.integer = 1 },
        { .type = AP_PLIST_STRING, .as.bytes = { "two", 3 } },
        { .type = AP_PLIST_BOOL, .as.boolean = false },
        nested_dict,
    };
    ap_plist_value_t list_value = { .type = AP_PLIST_ARRAY, .as.array = { list_items, 4 } };

    ap_plist_entry_t root_entries[10] = {
        { "string", &string_value }, { "empty", &empty_value },   { "small", &small_value },
        { "big", &big_value },       { "negative", &negative_value },
        { "flag", &true_value },     { "off", &false_value },     { "ratio", &real_value },
        { "blob", &data_value },     { "list", &list_value },
    };
    ap_plist_value_t root = { .type = AP_PLIST_DICT, .as.dict = { root_entries, 10 } };

    uint8_t buffer[2048];
    size_t len = ap_plist_encode(&root, buffer, sizeof(buffer));
    CHECK(len > 0);
    CHECK(memcmp(buffer, "bplist00", 8) == 0);
    write_file(directory, "encoded.bplist", buffer, len);

    /* Too small a destination must be rejected, not truncated. */
    CHECK(ap_plist_encode(&root, buffer, 16) == 0);

    /* Round-trip through our own decoder. */
    ap_plist_reader_t reader;
    CHECK(ap_plist_decode(&reader, buffer, len));
    uint64_t root_object = ap_plist_root(&reader);
    uint64_t value;
    CHECK(ap_plist_dict_uint(&reader, root_object, "big", &value));
    CHECK(value == 1234567890123LL);
    int64_t signed_value;
    CHECK(ap_plist_dict_int(&reader, root_object, "negative", &signed_value));
    CHECK(signed_value == -7);
    bool flag;
    CHECK(ap_plist_dict_bool(&reader, root_object, "flag", &flag) && flag);
    uint64_t list;
    CHECK(ap_plist_dict_get(&reader, root_object, "list", &list));
    CHECK(ap_plist_array_count(&reader, list) == 4);
    uint64_t nested;
    CHECK(ap_plist_array_get(&reader, list, 3, &nested));
    CHECK(ap_plist_dict_uint(&reader, nested, "nested", &value) && value == 3);
    CHECK(!ap_plist_dict_get(&reader, root_object, "missing", &value));
    CHECK(!ap_plist_decode(&reader, "not a plist at all", 16));
}

static void test_setup_bodies(const char *directory)
{
    printf("test_setup_bodies\n");

    uint8_t shk[16];
    uint8_t shiv[16];
    for (int i = 0; i < 16; i++) {
        shk[i] = (uint8_t)i;
        shiv[i] = (uint8_t)(i + 16);
    }

    ap_setup_params_t params = {
        .device_id = "02:11:22:33:44:55",
        .session_uuid = "0F0E0D0C-0B0A-4908-8706-050403020100",
        .name = "ESP32-S3 AirPlay",
        .timing_port = 50000,
        .audio_control_port = 50001,
        .audio_stream_id = 1111111111111111111ULL,
        .video_stream_id = 2222222222222222222ULL,
        .shk = shk,
        .shiv = shiv,
    };

    uint8_t buffer[1024];

    size_t len = ap_setup_build_control(&params, buffer, sizeof(buffer));
    CHECK(len > 0);
    write_file(directory, "control.bplist", buffer, len);

    /* The same descriptor with PTP timing: the receiver is named as a timing
     * peer instead of a local NTP port being advertised. */
    params.use_ptp = true;
    params.timing_peer_id = "9A8B7C6D-5E4F-4A3B-8C2D-1E0F9A8B7C6D";
    params.timing_peer_address = "10.0.0.7";
    len = ap_setup_build_control(&params, buffer, sizeof(buffer));
    CHECK(len > 0);
    write_file(directory, "control_ptp.bplist", buffer, len);
    params.use_ptp = false;
    params.timing_peer_id = NULL;
    params.timing_peer_address = NULL;

    len = ap_setup_build_audio(&params, buffer, sizeof(buffer));
    CHECK(len > 0);
    write_file(directory, "audio.bplist", buffer, len);

    params.audio_stream_connections = true;
    len = ap_setup_build_audio(&params, buffer, sizeof(buffer));
    CHECK(len > 0);
    write_file(directory, "audio_connections.bplist", buffer, len);
    params.audio_stream_connections = false;

    len = ap_setup_build_video(&params, buffer, sizeof(buffer));
    CHECK(len > 0);
    write_file(directory, "video.bplist", buffer, len);

    /* A reply to a video SETUP must be read back through the same helpers the
     * session uses. */
    ap_plist_reader_t reader;
    CHECK(ap_plist_decode(&reader, buffer, len));
    uint64_t streams;
    uint64_t root = ap_plist_root(&reader);
    CHECK(ap_plist_dict_get(&reader, root, "streams", &streams));
    uint64_t stream;
    CHECK(ap_plist_array_get(&reader, streams, 0, &stream));
    uint64_t type;
    CHECK(ap_plist_dict_uint(&reader, stream, "type", &type) && type == 110);
    uint64_t stream_id;
    CHECK(ap_plist_dict_uint(&reader, stream, "streamConnectionID", &stream_id));
    CHECK(stream_id == 2222222222222222222ULL);
}

static void test_decode_python_samples(const char *directory)
{
    printf("test_decode_python_samples\n");

    static uint8_t buffer[2048];

    size_t len = read_file(directory, "sample_types.bplist", buffer, sizeof(buffer));
    CHECK(len > 0);

    ap_plist_reader_t reader;
    CHECK(ap_plist_decode(&reader, buffer, len));
    uint64_t root = ap_plist_root(&reader);

    uint64_t small;
    CHECK(ap_plist_dict_uint(&reader, root, "small", &small) && small == 42);
    uint64_t large;
    CHECK(ap_plist_dict_uint(&reader, root, "large", &large) && large == 1234567890123ULL);
    int64_t negative;
    CHECK(ap_plist_dict_int(&reader, root, "negative", &negative) && negative == -7);
    bool flag;
    CHECK(ap_plist_dict_bool(&reader, root, "flag", &flag) && flag);
    CHECK(ap_plist_dict_bool(&reader, root, "off", &flag) && !flag);

    uint64_t list;
    CHECK(ap_plist_dict_get(&reader, root, "list", &list));
    CHECK(ap_plist_array_count(&reader, list) == 4);
    uint64_t item;
    CHECK(ap_plist_array_get(&reader, list, 0, &item));
    uint64_t number;
    CHECK(ap_plist_uint(&reader, item, &number) && number == 1);
    CHECK(ap_plist_array_get(&reader, list, 3, &item));
    uint64_t nested;
    CHECK(ap_plist_dict_get(&reader, item, "nested", &nested));
    CHECK(ap_plist_uint(&reader, nested, &number) && number == 3);

    /* GET /info reply. */
    len = read_file(directory, "sample_info.bplist", buffer, sizeof(buffer));
    CHECK(len > 0);
    ap_receiver_reply_t info;
    CHECK(ap_setup_parse_reply(buffer, len, &info));
    CHECK(info.has_features && info.features == 0x5A7FFFF7ULL);
    CHECK(info.has_status_flags && (info.status_flags & AP_STATUS_FLAG_PIN) != 0);
    CHECK(info.display_count == 2);
    CHECK(info.display_width == 1920 && info.display_height == 1080);

    /* Control SETUP reply. */
    len = read_file(directory, "sample_setup.bplist", buffer, sizeof(buffer));
    CHECK(len > 0);
    ap_receiver_reply_t setup;
    CHECK(ap_setup_parse_reply(buffer, len, &setup));
    CHECK(setup.has_event_port && setup.event_port == 7001);
    CHECK(setup.skip_record);
    CHECK(setup.has_video_data_port && setup.video_data_port == 7100);
    CHECK(setup.has_audio_data_port && setup.audio_data_port == 7200);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : "out";

    test_encode_all_types(directory);
    test_setup_bodies(directory);
    test_decode_python_samples(directory);

    if (failures != 0) {
        printf("test_plist: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_plist: ok\n");
    return 0;
}
