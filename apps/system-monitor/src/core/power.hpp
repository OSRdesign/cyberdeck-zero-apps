/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the temperature state and the power flags in plain words (no LVGL; tests/test_stats.cpp).
 *
 * Temperature: OK under 70 °C, warm from 70 °C, hot from 80 °C (the Raspberry Pi firmware starts slowing the CPU at
 * 80 °C and throttles hard at 85 °C).
 *
 * Power flags: the firmware's get_throttled bits (vcgencmd get_throttled, or the firmware's sysfs node):
 *   bit 0  under-voltage now          bit 16  under-voltage has occurred since boot
 *   bit 1  ARM frequency capped now   bit 17  frequency capping has occurred
 *   bit 2  throttled now              bit 18  throttling has occurred
 *   bit 3  soft temperature limit now bit 19  soft temperature limit has occurred
 * The firmware keeps no time for an event: the app notes the uptime of the last sample in which a flag was "now" (or
 * in which an "occurred" bit appeared), which is the time since boot of the last event it saw.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sm {

enum class Tone { Normal, Warn, Error, Good }; /* Good: the green of a state that is fine (Power OK, a cool CPU) */

/* ---------------------------------------------------------------- temperature */

constexpr double kWarmC = 70.0;
constexpr double kHotC = 80.0;

enum class TempState { Ok, Warm, Hot };

TempState temp_state(double celsius);
Tone temp_tone(TempState state);
const char *temp_state_word(TempState state); /* "OK", "Warm", "Hot" */

/* ---------------------------------------------------------------- power flags */

enum Flag { kUnderVoltage = 0, kFreqCapped = 1, kThrottled = 2, kSoftTempLimit = 3, kFlagCount = 4 };

const char *flag_name(int flag); /* "Under-voltage", "Frequency capped", "Throttled", "Soft temp limit" */

struct FlagState {
    bool now = false;
    bool occurred = false;      /* since boot (the firmware's sticky bit) */
    bool seen = false;          /* the app saw the event (now, or the sticky bit appearing) */
    double last_s = -1;         /* uptime of the last sample showing it, -1 when not seen */
};

/* Where the power information comes from. */
enum class PowerSource {
    None,      /* nothing on this system: the row is left out */
    Firmware,  /* get_throttled (vcgencmd or the firmware's sysfs node) */
    Alarm,     /* only the under-voltage alarm of the rpi_volt hwmon (no history, no other flags) */
    NoAccess,  /* vcgencmd is there but refused: not in the video group */
};

struct PowerInfo {
    PowerSource source = PowerSource::None;
    std::string via;            /* "vcgencmd", "sysfs", "hwmon" */
    std::uint32_t bits = 0;     /* Firmware */
    FlagState flags[kFlagCount];
    bool supply_ok = false;     /* the 5 V input (Pi 5 / CM5: vcgencmd pmic_read_adc EXT5V_V) */
    double supply_v = 0;
    bool core_ok = false;       /* the SoC core voltage (vcgencmd measure_volts core) */
    double core_v = 0;
};

/* Follows the flags from sample to sample. */
class ThrottleTracker {
public:
    /* A get_throttled value read at this uptime. */
    void update(std::uint32_t bits, double uptime_s);
    /* Only the hwmon under-voltage alarm (no sticky bit from the firmware: "occurred" is what the app saw). */
    void update_alarm(bool under_voltage_now, double uptime_s);
    void fill(PowerInfo &info) const;
    bool valid() const { return valid_; }

private:
    bool valid_ = false;
    std::uint32_t bits_ = 0;
    FlagState flags_[kFlagCount];
};

/* The sentence for the power state, most urgent first:
 *   "Under-voltage now: check the power supply" (Error), "Throttled now: the CPU is too hot" (Error, or "Throttled now"
 *   when not hot), "Soft temperature limit now: the CPU runs slower" (Warn), "Frequency capped now" (Warn),
 *   "Under-voltage happened since boot" (Warn), "Throttled since boot" ... (Warn), "Power OK" (Normal);
 *   NoAccess: "Needs the video group: sudo usermod -aG video $USER" (Warn; then log in again);
 *   None: "" */
struct Status {
    std::string text;
    Tone tone = Tone::Normal;
};

Status power_status(const PowerInfo &info, bool cpu_hot);
/* A flag's row value: {"now"}, {"last 0:41:12 after boot", "last at 0:41:12", "0:41:12"}, {"before the app started",
 * "since boot"}, {"never since boot", "never"} */
std::vector<std::string> flag_values(const FlagState &flag, bool alarm_only);

} // namespace sm
