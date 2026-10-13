/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: temperature state and power wording (see power.hpp).
 */
#include "power.hpp"

#include "text.hpp"

namespace sm {

TempState temp_state(double celsius)
{
    if (celsius >= kHotC) return TempState::Hot;
    if (celsius >= kWarmC) return TempState::Warm;
    return TempState::Ok;
}

Tone temp_tone(TempState state)
{
    switch (state) {
    case TempState::Hot: return Tone::Error;
    case TempState::Warm: return Tone::Warn;
    default: return Tone::Normal;
    }
}

const char *temp_state_word(TempState state)
{
    switch (state) {
    case TempState::Hot: return "Hot";
    case TempState::Warm: return "Warm";
    default: return "OK";
    }
}

const char *flag_name(int flag)
{
    switch (flag) {
    case kUnderVoltage: return "Under-voltage";
    case kFreqCapped: return "Frequency capped";
    case kThrottled: return "Throttled";
    case kSoftTempLimit: return "Soft temp limit";
    default: return "?";
    }
}

void ThrottleTracker::update(std::uint32_t bits, double uptime_s)
{
    for (int f = 0; f < kFlagCount; ++f) {
        FlagState &s = flags_[f];
        const bool now = (bits >> f) & 1u;
        const bool occurred = ((bits >> (16 + f)) & 1u) || now;
        const bool appeared = valid_ && occurred && !s.occurred; /* happened between two samples */
        if (now || appeared) {
            s.seen = true;
            s.last_s = uptime_s;
        }
        s.now = now;
        s.occurred = occurred;
    }
    bits_ = bits;
    valid_ = true;
}

void ThrottleTracker::update_alarm(bool under_voltage_now, double uptime_s)
{
    FlagState &s = flags_[kUnderVoltage];
    if (under_voltage_now) {
        s.seen = true;
        s.occurred = true;
        s.last_s = uptime_s;
    }
    s.now = under_voltage_now;
    bits_ = under_voltage_now ? 1u : 0u;
    valid_ = true;
}

void ThrottleTracker::fill(PowerInfo &info) const
{
    info.bits = bits_;
    for (int f = 0; f < kFlagCount; ++f) info.flags[f] = flags_[f];
}

Status power_status(const PowerInfo &info, bool cpu_hot)
{
    Status s;
    if (info.source == PowerSource::None) return s;
    if (info.source == PowerSource::NoAccess) {
        s.text = "Needs the video group: sudo usermod -aG video $USER";
        s.tone = Tone::Warn;
        return s;
    }
    const FlagState *f = info.flags;
    if (f[kUnderVoltage].now) return {"Under-voltage now: check the power supply", Tone::Error};
    if (f[kThrottled].now)
        return {cpu_hot ? "Throttled now: the CPU is too hot" : "Throttled now: the CPU runs slower", Tone::Error};
    if (f[kSoftTempLimit].now) return {"Soft temperature limit now: the CPU runs slower", Tone::Warn};
    if (f[kFreqCapped].now) return {"Frequency capped now: the CPU runs slower", Tone::Warn};
    if (f[kUnderVoltage].occurred) return {"Under-voltage happened since boot", Tone::Warn};
    if (f[kThrottled].occurred) return {"Throttling happened since boot", Tone::Warn};
    if (f[kSoftTempLimit].occurred) return {"Soft temperature limit happened since boot", Tone::Warn};
    if (f[kFreqCapped].occurred) return {"Frequency capping happened since boot", Tone::Warn};
    return {"Power OK", Tone::Normal};
}

std::vector<std::string> power_values(const PowerInfo &info, Tone &tone)
{
    tone = Tone::Normal;
    if (info.source == PowerSource::None) return {"Unknown"};
    if (info.source == PowerSource::NoAccess) {
        tone = Tone::Warn;
        return {"Needs the video group", "No access"};
    }
    const FlagState *f = info.flags;
    if (f[kUnderVoltage].now) {
        tone = Tone::Error;
        return {"Under-voltage now", "UV now"};
    }
    if (f[kThrottled].now) {
        tone = Tone::Error;
        return {"Throttled now"};
    }
    if (f[kSoftTempLimit].now) {
        tone = Tone::Warn;
        return {"Temp limit now"};
    }
    if (f[kFreqCapped].now) {
        tone = Tone::Warn;
        return {"Capped now"};
    }
    if (f[kUnderVoltage].occurred) {
        tone = Tone::Warn;
        return {"Under-voltage since boot", "UV since boot", "UV earlier"};
    }
    if (f[kThrottled].occurred || f[kSoftTempLimit].occurred || f[kFreqCapped].occurred) {
        tone = Tone::Warn;
        return {"Throttled since boot", "Slowed earlier"};
    }
    return {"OK"};
}

std::vector<std::string> flag_values(const FlagState &flag, bool alarm_only)
{
    if (flag.now) return {"now"};
    if (flag.seen && flag.last_s >= 0) {
        const std::string t = fmt_since_boot(flag.last_s);
        return {"last " + t + " after boot", "last at " + t, t};
    }
    if (flag.occurred) return {"before the app started", "since boot"};
    if (alarm_only) return {"not seen"};
    return {"never since boot", "never"};
}

Status overview_status(const PowerInfo &power, bool temp_ok, double temp_c)
{
    const bool hot = temp_ok && temp_state(temp_c) == TempState::Hot;
    const bool warm = temp_ok && temp_state(temp_c) == TempState::Warm;
    const Status p = power_status(power, hot);
    if (p.tone == Tone::Error) return p;
    if (hot) return {"CPU hot: " + fmt_temp_short(temp_c) + ", the Pi slows down from 80 to 85 \xC2\xB0" "C",
                     Tone::Error};
    if (p.tone == Tone::Warn) return p;
    if (warm) return {"CPU warm: " + fmt_temp_short(temp_c) + " (hot from 80 \xC2\xB0" "C)", Tone::Warn};
    return p;
}

} // namespace sm
