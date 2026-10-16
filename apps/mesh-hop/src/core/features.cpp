/*
 * SPDX-License-Identifier: MIT
 */

#include "features.hpp"

#include <cstdio>

namespace meshzero {

int feature_min_level(Feature f)
{
    switch (f) {
    case Feature::ClientRepeat: return 9;
    case Feature::PathHashMode: return 10;
    default: return 0;
    }
}

const char *feature_name(Feature f)
{
    switch (f) {
    case Feature::BoardGps: return "Board GPS";
    case Feature::ChannelAdmin: return "Channel management";
    case Feature::ClientRepeat: return "Client repeat";
    case Feature::PathHashMode: return "Path hash size";
    case Feature::AutoAdd: return "Auto-add contacts";
    case Feature::Stats: return "Statistics";
    case Feature::Discover: return "Discover nearby nodes";
    }
    return "";
}

FeatureState feature_state(Feature f, const DeviceInfo *dev, const BoardCaps &caps)
{
    FeatureState s;
    const std::string old = std::string("Firmware too old for this feature: ") + feature_name(f);
    switch (f) {
    case Feature::BoardGps:
        if (caps.custom_vars == Cap::No) s.notice = old;
        else if (caps.custom_vars == Cap::Yes && caps.gps_listed) s.available = true;
        else s.hidden = true;                    // not asked yet, or a board that lists no gps variable: no GPS to switch
        break;
    case Feature::ChannelAdmin:
        if (caps.channels == Cap::No) s.notice = old;
        else s.available = true;
        break;
    case Feature::ClientRepeat:
    case Feature::PathHashMode:
        if (!dev) { s.hidden = true; break; }
        if (dev->fw_ver >= feature_min_level(f)) s.available = true;
        else s.notice = old + " (needs firmware level " + std::to_string(feature_min_level(f)) + ", the board has " + std::to_string(dev->fw_ver) + ")";
        break;
    case Feature::AutoAdd:
        if (caps.autoadd == Cap::No) s.notice = old;
        else if (caps.autoadd == Cap::Yes) s.available = true;
        else s.hidden = true;                    // not asked yet (not connected)
        break;
    case Feature::Stats:
        if (caps.stats == Cap::No) s.notice = old;
        else if (dev) s.available = true;        // asked when the screen opens: an unsupported answer then turns it off
        else s.hidden = true;
        break;
    case Feature::Discover:
        if (caps.discover == Cap::No) s.notice = old;
        else s.available = true;
        break;
    }
    return s;
}

bool repeat_freq_allowed(const std::vector<RepeatRange> &ranges, double freq_mhz)
{
    // The references do not name the unit of GET_ALLOWED_REPEAT_FREQ; the radio frequency of SET_RADIO is in kHz and so are taken the ranges.
    // A value below 10000 cannot be kHz of a LoRa band (10 MHz): it is read as MHz, in case the firmware answers that way.
    const double khz = freq_mhz * 1000.0;
    for (const RepeatRange &r : ranges) {
        const double scale = r.hi_khz < 10000 ? 1000.0 : 1.0;
        if (khz >= r.lo_khz * scale - 0.5 && khz <= r.hi_khz * scale + 0.5) return true;
    }
    return false;
}

std::string repeat_ranges_text(const std::vector<RepeatRange> &ranges)
{
    std::string t;
    char b[64];
    for (const RepeatRange &r : ranges) {
        const double div = r.hi_khz < 10000 ? 1.0 : 1000.0;
        std::snprintf(b, sizeof(b), "%.3f-%.3f", r.lo_khz / div, r.hi_khz / div);
        if (!t.empty()) t += ", ";
        t += b;
    }
    return t.empty() ? "none" : t + " MHz";
}

RepeatState repeat_state(const DeviceInfo *dev, const BoardCaps &caps, double freq_mhz)
{
    RepeatState s;
    if (!dev) return s;
    s.shown = true;
    s.on = dev->has_repeat && dev->repeat;
    if (dev->fw_ver < feature_min_level(Feature::ClientRepeat) || !dev->has_repeat) {
        s.note = "Firmware too old for this feature: Client repeat (needs firmware level 9, the board has " + std::to_string(dev->fw_ver) + ")";
        return s;
    }
    if (caps.repeat_freqs == Cap::No) {
        s.note = "Firmware too old for this feature: allowed repeat frequencies";
        return s;
    }
    if (caps.repeat_freqs != Cap::Yes) {
        s.note = "Reading the allowed frequencies from the board...";
        return s;
    }
    if (!repeat_freq_allowed(caps.repeat_ranges, freq_mhz)) {
        s.note = "Not allowed on this frequency (allowed: " + repeat_ranges_text(caps.repeat_ranges) + ")";
        return s;
    }
    s.available = true;
    return s;
}

int path_hash_bytes(int mode)
{
    return mode >= 0 && mode <= 2 ? mode + 1 : 0;
}

const char *path_hash_text(int mode)
{
    switch (mode) {
    case 0: return "1 byte";
    case 1: return "2 bytes";
    case 2: return "3 bytes";
    default: return "unknown";
    }
}

std::string firmware_text(const DeviceInfo *dev)
{
    if (!dev) return "-";
    std::string t = dev->version.empty() ? std::string("unknown version") : dev->version;
    return t + " (level " + std::to_string(dev->fw_ver) + ")";
}

} // namespace meshzero
