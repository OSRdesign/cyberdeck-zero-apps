/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the under-voltage notice (brief 5.6). While the camera is open, the firmware's get_throttled bit 0 (under-voltage
 * now; the spike's reader, live/stats.hpp SystemProbe::throttled) or the rpi_volt hwmon's in0_lcrit_alarm
 * (SystemProbe::undervolt_alarm) gives the notice "Power too weak for the camera." at most once every 60 s. Both sources
 * are optional and absent on amd64. Nothing is ever stopped because of it. Pure (unit-tested). LVGL-free.
 */
#pragma once

#include <cstdint>

namespace vhs {

class UndervoltNotice {
public:
    explicit UndervoltNotice(std::int64_t every_ms = 60000) : every_ms_(every_ms) {}
    /* One reading (under_now) at now_ms: true when the notice is to be shown now. */
    bool step(bool under_now, std::int64_t now_ms)
    {
        if (!under_now) return false;
        if (shown_ && now_ms - last_ms_ < every_ms_) return false;
        shown_ = true;
        last_ms_ = now_ms;
        return true;
    }

private:
    std::int64_t every_ms_;
    std::int64_t last_ms_ = 0;
    bool shown_ = false;
};

} // namespace vhs
