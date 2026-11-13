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

void alsa_fix_mute_db(CtlInfo &c, bool next_known, long db_next_cdb) { /* VINYL */
    if (!c.has_db || c.db_min_cdb > kAlsaDbGainMute) return;
    c.min_is_mute = true;
    if (!next_known || c.max <= c.min + 1 || c.db_max_cdb <= db_next_cdb) {
        c.has_db = false; /* no real dB for min: linear in steps */
        return;
    }
    const long step = (c.db_max_cdb - db_next_cdb) / (c.max - c.min - 1); /* hundredths of a dB a value */
    c.db_min_cdb = db_next_cdb - step;
}

} // namespace vinyl
