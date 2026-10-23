#include "ap_setup.h"

#include <string.h>

#include "ap_plist.h"

/* The SETUP bodies are built from one task at a time; small static pools keep
 * the several-hundred-byte descriptor off the caller's stack. */
#define AP_SETUP_VALUE_POOL 48
#define AP_SETUP_ENTRY_POOL 32

static ap_plist_value_t s_values[AP_SETUP_VALUE_POOL];
static ap_plist_entry_t s_entries[AP_SETUP_ENTRY_POOL];
static size_t s_value_used;
static size_t s_entry_used;

static void pool_reset(void)
{
    s_value_used = 0;
    s_entry_used = 0;
}

static ap_plist_value_t *value_slots(size_t count)
{
    if (s_value_used + count > AP_SETUP_VALUE_POOL) {
        return NULL;
    }
    ap_plist_value_t *first = &s_values[s_value_used];
    s_value_used += count;
    memset(first, 0, count * sizeof(*first));
    return first;
}

static ap_plist_entry_t *entry_slots(size_t count)
{
    if (s_entry_used + count > AP_SETUP_ENTRY_POOL) {
        return NULL;
    }
    ap_plist_entry_t *first = &s_entries[s_entry_used];
    s_entry_used += count;
    return first;
}

static ap_plist_value_t *make_string(const char *text)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_STRING;
    value->as.bytes.ptr = text;
    value->as.bytes.len = strlen(text);
    return value;
}

static ap_plist_value_t *make_int(int64_t number)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_INT;
    value->as.integer = number;
    return value;
}

static ap_plist_value_t *make_bool(bool flag)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_BOOL;
    value->as.boolean = flag;
    return value;
}

static ap_plist_value_t *make_data(const void *bytes, size_t len)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_DATA;
    value->as.bytes.ptr = bytes;
    value->as.bytes.len = len;
    return value;
}

static ap_plist_value_t *make_dict(ap_plist_entry_t *entries, size_t count)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_DICT;
    value->as.dict.entries = entries;
    value->as.dict.count = count;
    return value;
}

static ap_plist_value_t *make_array(ap_plist_value_t *items, size_t count)
{
    ap_plist_value_t *value = value_slots(1);
    if (value == NULL) {
        return NULL;
    }
    value->type = AP_PLIST_ARRAY;
    value->as.array.items = items;
    value->as.array.count = count;
    return value;
}

#define SET_ENTRY(slot, key_text, value_expr) \
    do {                                      \
        ap_plist_entry_t *entry_ = &(slot);   \
        entry_->key = (key_text);             \
        entry_->value = (value_expr);         \
        if (entry_->value == NULL) {          \
            return 0;                         \
        }                                     \
    } while (0)

size_t ap_setup_build_control(const ap_setup_params_t *params, void *out, size_t cap)
{
    pool_reset();
    ap_plist_entry_t *entries = entry_slots(14);
    if (entries == NULL) {
        return 0;
    }

    size_t n = 0;
    SET_ENTRY(entries[n++], "deviceID", make_string(params->device_id));
    SET_ENTRY(entries[n++], "macAddress", make_string(params->device_id));
    SET_ENTRY(entries[n++], "sessionUUID", make_string(params->session_uuid));
    SET_ENTRY(entries[n++], "sourceVersion", make_string("980.71.1"));
    SET_ENTRY(entries[n++], "isScreenMirroringSession", make_bool(true));
    SET_ENTRY(entries[n++], "timingProtocol", make_string(params->use_ptp ? "PTP" : "NTP"));
    SET_ENTRY(entries[n++], "osBuildVersion", make_string("13F69"));
    SET_ENTRY(entries[n++], "model", make_string("Linux"));
    SET_ENTRY(entries[n++], "name", make_string(params->name));
    if (params->use_ptp) {
        /* The receiver's own timeline is authoritative; naming ourselves as a
         * timing peer is how an encrypted session says so. */
        ap_plist_value_t *addresses = value_slots(1);
        ap_plist_entry_t *peer = entry_slots(4);
        ap_plist_value_t *peers = value_slots(1);
        if (addresses == NULL || peer == NULL || peers == NULL) {
            return 0;
        }
        addresses[0] = *make_string(params->timing_peer_address);
        SET_ENTRY(peer[0], "ID", make_string(params->timing_peer_id));
        SET_ENTRY(peer[1], "SupportsClockPortMatchingOverride", make_bool(true));
        SET_ENTRY(peer[2], "DeviceType", make_int(0));
        SET_ENTRY(peer[3], "Addresses", make_array(addresses, 1));
        peers[0] = *make_dict(peer, 4);
        SET_ENTRY(entries[n++], "timingPeerInfo", make_dict(peer, 4));
        SET_ENTRY(entries[n++], "timingPeerList", make_array(peers, 1));
    } else {
        SET_ENTRY(entries[n++], "timingPort", make_int(params->timing_port));
    }
    SET_ENTRY(entries[n++], "updateSessionRequest", make_bool(false));
    SET_ENTRY(entries[n++], "combinedGetInfoWithControlSetup", make_bool(true));

    return ap_plist_encode(make_dict(entries, n), out, cap);
}

size_t ap_setup_build_audio(const ap_setup_params_t *params, void *out, size_t cap)
{
    pool_reset();
    ap_plist_value_t *items = value_slots(1);
    ap_plist_entry_t *root_entries = entry_slots(1);
    ap_plist_entry_t *stream = entry_slots(16);
    if (items == NULL || root_entries == NULL || stream == NULL) {
        return 0;
    }

    size_t n = 0;
    SET_ENTRY(stream[n++], "type", make_int(96));
    SET_ENTRY(stream[n++], "streamConnectionID", make_int((int64_t)params->audio_stream_id));
    /* ALAC placeholder: no audio is transmitted, but the receiver expects a
     * screen-audio descriptor before it reaches its rendering state. */
    SET_ENTRY(stream[n++], "ct", make_int(2));
    SET_ENTRY(stream[n++], "spf", make_int(352));
    SET_ENTRY(stream[n++], "sr", make_int(44100));
    SET_ENTRY(stream[n++], "audioFormat", make_int(0x40000));
    SET_ENTRY(stream[n++], "audioMode", make_string("default"));
    SET_ENTRY(stream[n++], "usingScreen", make_bool(true));
    SET_ENTRY(stream[n++], "latencyMin", make_int(0));
    SET_ENTRY(stream[n++], "latencyMax", make_int(3748));
    SET_ENTRY(stream[n++], "redundantAudio", make_int(2));
    if (params->audio_stream_connections) {
        /* Modern layout: the connection dictionaries carry the port and say
         * whether the RTP stream is encrypted. No audio key is published
         * because no audio packets are ever sent. */
        ap_plist_entry_t *rtp_entries = entry_slots(1);
        ap_plist_entry_t *rtcp_entries = entry_slots(1);
        ap_plist_entry_t *connection_entries = entry_slots(2);
        if (rtp_entries == NULL || rtcp_entries == NULL || connection_entries == NULL) {
            return 0;
        }
        SET_ENTRY(rtp_entries[0], "streamConnectionKeyUseStreamEncryptionKey",
                  make_bool(params->audio_shk != NULL));
        SET_ENTRY(rtcp_entries[0], "streamConnectionKeyPort", make_int(params->audio_control_port));
        SET_ENTRY(connection_entries[0], "streamConnectionTypeRTP", make_dict(rtp_entries, 1));
        SET_ENTRY(connection_entries[1], "streamConnectionTypeRTCP", make_dict(rtcp_entries, 1));
        SET_ENTRY(stream[n++], "isMedia", make_bool(false));
        SET_ENTRY(stream[n++], "supportsDynamicStreamID", make_bool(true));
        SET_ENTRY(stream[n++], "streamConnections", make_dict(connection_entries, 2));
        if (params->audio_shk != NULL) {
            SET_ENTRY(stream[n++], "shk", make_data(params->audio_shk, 32));
        }
    } else {
        SET_ENTRY(stream[n++], "controlPort", make_int(params->audio_control_port));
        if (params->audio_shk != NULL) {
            SET_ENTRY(stream[n++], "shk", make_data(params->audio_shk, 32));
        }
    }

    items[0] = *make_dict(stream, n);
    SET_ENTRY(root_entries[0], "streams", make_array(items, 1));

    return ap_plist_encode(make_dict(root_entries, 1), out, cap);
}

size_t ap_setup_build_video(const ap_setup_params_t *params, void *out, size_t cap)
{
    static const char *const k_timestamp_names[5] = { "SubSu", "BePxT", "AfPxT", "BefEn", "EmEnc" };

    pool_reset();
    ap_plist_value_t *stream_items = value_slots(1);
    ap_plist_value_t *timestamp_items = value_slots(5);
    ap_plist_entry_t *root_entries = entry_slots(1);
    ap_plist_entry_t *stream = entry_slots(6);
    ap_plist_entry_t *timestamp_entries = entry_slots(5);
    if (stream_items == NULL || timestamp_items == NULL || root_entries == NULL || stream == NULL ||
        timestamp_entries == NULL) {
        return 0;
    }

    for (size_t i = 0; i < 5; i++) {
        timestamp_entries[i].key = "name";
        timestamp_entries[i].value = make_string(k_timestamp_names[i]);
        if (timestamp_entries[i].value == NULL) {
            return 0;
        }
        timestamp_items[i].type = AP_PLIST_DICT;
        timestamp_items[i].as.dict.entries = &timestamp_entries[i];
        timestamp_items[i].as.dict.count = 1;
    }

    size_t n = 0;
    SET_ENTRY(stream[n++], "type", make_int(110));
    SET_ENTRY(stream[n++], "streamConnectionID", make_int((int64_t)params->video_stream_id));
    SET_ENTRY(stream[n++], "latencyMs", make_int(75));
    SET_ENTRY(stream[n++], "timestampInfo", make_array(timestamp_items, 5));
    /* The receiver keys the video stream from these even when the frames
     * themselves arrive encrypted with the ChaCha20 DataStream key. */
    SET_ENTRY(stream[n++], "shk", make_data(params->shk, 16));
    SET_ENTRY(stream[n++], "shiv", make_data(params->shiv, 16));

    stream_items[0] = *make_dict(stream, n);
    SET_ENTRY(root_entries[0], "streams", make_array(stream_items, 1));

    return ap_plist_encode(make_dict(root_entries, 1), out, cap);
}

bool ap_setup_parse_reply(const void *body, size_t len, ap_receiver_reply_t *out)
{
    if (body == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    ap_plist_reader_t reader;
    if (!ap_plist_decode(&reader, body, len)) {
        return false;
    }
    uint64_t root = ap_plist_root(&reader);
    if (root == AP_PLIST_NONE) {
        return false;
    }

    uint64_t value;
    if (ap_plist_dict_uint(&reader, root, "eventPort", &value) && value > 0 && value <= 0xffff) {
        out->has_event_port = true;
        out->event_port = (uint16_t)value;
    }
    bool flag;
    if (ap_plist_dict_bool(&reader, root, "skipRecord", &flag)) {
        out->skip_record = flag;
    }
    if (ap_plist_dict_uint(&reader, root, "features", &value)) {
        out->has_features = true;
        out->features = value;
    }
    if (ap_plist_dict_uint(&reader, root, "statusFlags", &value)) {
        out->has_status_flags = true;
        out->status_flags = value;
    }

    uint64_t timing_peer;
    if (ap_plist_dict_get(&reader, root, "timingPeerInfo", &timing_peer)) {
        uint64_t clock_id;
        if (ap_plist_dict_uint(&reader, timing_peer, "ClockID", &clock_id)) {
            out->has_clock_id = true;
            out->clock_id = clock_id;
        }
    }

    uint64_t displays;
    if (ap_plist_dict_get(&reader, root, "displays", &displays)) {
        out->display_count = ap_plist_array_count(&reader, displays);
        uint64_t first;
        if (ap_plist_array_get(&reader, displays, 0, &first)) {
            uint64_t dimension;
            if (ap_plist_dict_uint(&reader, first, "widthPixels", &dimension)) {
                out->display_width = (uint32_t)dimension;
            }
            if (ap_plist_dict_uint(&reader, first, "heightPixels", &dimension)) {
                out->display_height = (uint32_t)dimension;
            }
        }
    }

    uint64_t streams;
    if (ap_plist_dict_get(&reader, root, "streams", &streams)) {
        size_t count = ap_plist_array_count(&reader, streams);
        for (size_t i = 0; i < count; i++) {
            uint64_t stream;
            if (!ap_plist_array_get(&reader, streams, i, &stream)) {
                continue;
            }
            uint64_t type;
            if (!ap_plist_dict_uint(&reader, stream, "type", &type)) {
                continue;
            }
            uint64_t port;
            if (type == 110 && ap_plist_dict_uint(&reader, stream, "dataPort", &port) && port > 0 &&
                port <= 0xffff) {
                out->has_video_data_port = true;
                out->video_data_port = (uint16_t)port;
            } else if (type == 96 && ap_plist_dict_uint(&reader, stream, "dataPort", &port) && port > 0 &&
                       port <= 0xffff) {
                out->has_audio_data_port = true;
                out->audio_data_port = (uint16_t)port;
            }
        }
    }

    return true;
}
