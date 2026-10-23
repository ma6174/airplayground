/*
 * H.264 bitstream plumbing shared by the media sources: pulling NAL units out
 * of an Annex-B stream, wrapping them into the AVCC (length-prefixed) framing
 * the AirPlay data channel sends, and assembling the decoder configuration
 * record a receiver needs before its first frame.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Walks the NAL units of an Annex-B byte stream. Usage:
 *
 *     media_nal_iter_t it;
 *     media_nal_begin(&it, stream, len);
 *     while (media_nal_next(&it)) { use it->type, it->offset, it->nal_len; }
 *
 * The iterator points into the caller's buffer and copies nothing.
 */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;  /* first payload byte of the current NAL unit */
    size_t nal_len; /* payload bytes of the current NAL unit */
    int type;       /* nal_unit_type of the current NAL unit */
} media_nal_iter_t;

void media_nal_begin(media_nal_iter_t *it, const uint8_t *data, size_t len);
bool media_nal_next(media_nal_iter_t *it);

/* Builds the AVCDecoderConfigurationRecord (with the four trailing bytes real
 * senders append) from one SPS and one PPS. Returns the number of bytes
 * written, or 0 when the record does not fit. */
size_t media_avcc_record(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len,
                         uint8_t *out, size_t out_cap);

/* Re-frames an Annex-B access unit as AVCC (4-byte big-endian NAL lengths).
 * Sets *idr when the stream contains an IDR slice. Returns false when the
 * output does not fit. */
bool media_avcc_wrap(const uint8_t *annexb, size_t annexb_len, uint8_t *out, size_t out_cap,
                     size_t *out_len, bool *idr);
