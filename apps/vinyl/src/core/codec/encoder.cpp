/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "encoder.hpp"

namespace vinyl {

bool valid_tag_name(const std::string &name) {
    if (name.empty() || name.size() > 64) return false;
    for (unsigned char c : name)
        if (c < 0x20 || c > 0x7D || c == '=') return false;
    return true;
}

namespace {

inline int32_t s16_at(const unsigned char *p) { return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8))); }
inline int32_t s24_at(const unsigned char *p) {
    const uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
    return static_cast<int32_t>(u << 8) >> 8; /* sign-extend from bit 23 */
}

}  // namespace

void pcm_to_int32(const void *in, unsigned bits, int32_t *out, size_t samples) {
    const unsigned char *p = static_cast<const unsigned char *>(in);
    if (bits == 24) {
        for (size_t i = 0; i < samples; ++i, p += 3) out[i] = s24_at(p);
    } else {
        for (size_t i = 0; i < samples; ++i, p += 2) out[i] = s16_at(p);
    }
}

void pcm_to_float(const void *in, unsigned bits, float *out, size_t samples) {
    const unsigned char *p = static_cast<const unsigned char *>(in);
    if (bits == 24) {
        for (size_t i = 0; i < samples; ++i, p += 3) out[i] = static_cast<float>(s24_at(p)) * (1.0f / 8388608.0f);
    } else {
        for (size_t i = 0; i < samples; ++i, p += 2) out[i] = static_cast<float>(s16_at(p)) * (1.0f / 32768.0f);
    }
}

void pcm_to_int16(const void *in, unsigned bits, int16_t *out, size_t samples) {
    const unsigned char *p = static_cast<const unsigned char *>(in);
    if (bits == 24) {
        for (size_t i = 0; i < samples; ++i, p += 3) out[i] = static_cast<int16_t>(s24_at(p) >> 8);
    } else {
        for (size_t i = 0; i < samples; ++i, p += 2) out[i] = static_cast<int16_t>(s16_at(p));
    }
}

}  // namespace vinyl
