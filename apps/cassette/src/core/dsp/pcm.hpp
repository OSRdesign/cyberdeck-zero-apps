/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: interleaved little-endian PCM sample access. Two sample formats are supported, both signed:
 *   bits == 16: S16_LE, 2 bytes per sample;
 *   bits == 24: S24_3LE, 24 bits packed in 3 bytes per sample.
 * Any other `bits` value is "unsupported": the DSP classes then pass the data through untouched.
 * Float conversion normalises by 2^(bits-1), so -32768 maps to -1.0 and +32767 to 32767/32768. LVGL-free.
 */
#pragma once

#include "../frame_sink.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cassette {
namespace pcm {

inline bool supported(uint16_t bits) { return bits == 16 || bits == 24; }
inline size_t bytes_per_sample(uint16_t bits) { return bits == 24 ? 3 : 2; }
inline size_t bytes_per_frame(const AudioFormat &f) { return bytes_per_sample(f.bits) * f.channels; }
inline int32_t max_pos(uint16_t bits) { return bits == 24 ? 8388607 : 32767; }
inline int32_t min_neg(uint16_t bits) { return bits == 24 ? -8388608 : -32768; }
inline float scale(uint16_t bits) { return bits == 24 ? 8388608.0f : 32768.0f; }

inline int32_t read(const uint8_t *p, uint16_t bits) {
    if (bits == 24) {
        uint32_t u = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
        if (u & 0x800000u) u |= 0xFF000000u;
        return int32_t(u);
    }
    return int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

inline void write(uint8_t *p, uint16_t bits, int32_t v) {
    uint32_t u = uint32_t(v);
    p[0] = uint8_t(u);
    p[1] = uint8_t(u >> 8);
    if (bits == 24) p[2] = uint8_t(u >> 16);
}

/* Rounds a normalised float to an integer sample, saturating at full scale; sets `clipped` when it saturated. */
inline int32_t quantize(float x, uint16_t bits, bool &clipped) {
    const float s = x * scale(bits);
    const float hi = float(max_pos(bits)), lo = float(min_neg(bits));
    if (s != s) { clipped = false; return 0; } /* NaN: never expected, never propagated */
    if (s >= hi + 0.5f) { clipped = true; return max_pos(bits); }
    if (s <= lo - 0.5f) { clipped = true; return min_neg(bits); }
    clipped = false;
    return int32_t(std::lround(s));
}

}  // namespace pcm
}  // namespace cassette
