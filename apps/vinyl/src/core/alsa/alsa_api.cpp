/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "alsa_api.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace vinyl {

std::string alsa_error_text(int err) {
    if (err == kNoAlsa) return "no ALSA library (libasound.so.2 not found)";
    if (err >= 0) return "no error";
    return std::strerror(-err);
}

const char *format_name(SampleFormat f) {
    switch (f) {
    case SampleFormat::S16_LE: return "S16";
    case SampleFormat::S24_3LE: return "S24_3LE";
    case SampleFormat::S32_LE: return "S32";
    }
    return "?";
}

unsigned format_bytes(SampleFormat f) {
    switch (f) {
    case SampleFormat::S16_LE: return 2;
    case SampleFormat::S24_3LE: return 3;
    case SampleFormat::S32_LE: return 4;
    }
    return 2;
}

bool CaptureOffer::has_rate(uint32_t r) const { return std::find(rates.begin(), rates.end(), r) != rates.end(); }
bool CaptureOffer::has_format(SampleFormat f) const { return std::find(formats.begin(), formats.end(), f) != formats.end(); }

} // namespace vinyl
