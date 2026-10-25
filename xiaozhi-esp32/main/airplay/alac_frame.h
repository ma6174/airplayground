#ifndef AIRPLAY_ALAC_FRAME_H
#define AIRPLAY_ALAC_FRAME_H

#include <cstddef>
#include <cstdint>
#include <cstring>

/*
 * One ALAC frame in its uncompressed form, the codec the legacy AirPlay audio
 * channel declares: a stereo element tag, the sample count, then the samples
 * interleaved MSB-first (libavcodec/alac.c decode_element). The reference
 * implementations this port stays byte-identical with are tools/file_stream.py
 * and capture-cli's alacFrame.
 *
 * The frame is zero-padded to a 16-byte boundary: the protocol stack encrypts
 * whole AES blocks, and a receiver that decrypts the entire UDP payload instead
 * of stopping at the declared sample count would garble the tail of every
 * frame - a periodic crackle at 125 frames/s. Decoders stop after the declared
 * samples, so the padding is never heard.
 */

constexpr int kAlacSpf = 352;             // samples per channel per packet
constexpr size_t kAlacFrameBytes = 1424;  // 7 header bytes + 1408 samples + 9 pad

/* Builds one frame from kAlacSpf 44.1 kHz samples; the source is mono, so the
 * same value feeds both channels. Returns kAlacFrameBytes. */
inline size_t AlacBuildFrame(const int16_t* mono, uint8_t* out) {
    uint64_t acc = 0;
    int bits = 0;
    size_t pos = 0;
    auto write = [&](uint32_t value, int nbits) {
        acc = (acc << nbits) |
              (value & (nbits == 32 ? 0xFFFFFFFFu : ((1u << nbits) - 1)));
        bits += nbits;
        while (bits >= 8) {
            bits -= 8;
            out[pos++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
        }
        acc &= (1u << bits) - 1;
    };
    write(1, 3);    // element tag: stereo pair
    write(0, 4);    // element instance tag
    write(0, 12);   // unused
    write(1, 1);    // has_size: the sample count follows
    write(0, 2);    // extra bits field
    write(1, 1);    // is not compressed
    write(kAlacSpf, 32);
    for (int s = 0; s < kAlacSpf; s++) {
        const uint16_t value = static_cast<uint16_t>(mono[s]);
        write(value, 16);  // left
        write(value, 16);  // right
    }
    if (bits > 0) {
        out[pos++] = static_cast<uint8_t>((acc << (8 - bits)) & 0xFF);
    }
    memset(out + pos, 0, kAlacFrameBytes - pos);
    return kAlacFrameBytes;
}

#endif  // AIRPLAY_ALAC_FRAME_H
