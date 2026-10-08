/*
 * SPDX-License-Identifier: MIT
 */

#include "features.hpp"

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
    case Feature::PathHashMode: return "Path hash mode";
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
    }
    return s;
}

std::string firmware_text(const DeviceInfo *dev)
{
    if (!dev) return "-";
    std::string t = dev->version.empty() ? std::string("unknown version") : dev->version;
    return t + " (level " + std::to_string(dev->fw_ver) + ")";
}

} // namespace meshzero
