/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the words and numbers the rows show (see format.hpp).
 */
#include "format.hpp"

#include "text.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace ha {

std::string clean_unit(const std::string &unit)
{
    std::string out;
    for (std::size_t i = 0; i < unit.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(unit[i]);
        if (c < 0x80) out += static_cast<char>(c);
        /* other bytes (the degree sign C2 B0, micro, superscripts) are left out */
    }
    return trim(out);
}

std::string format_number(const std::string &raw, int precision)
{
    if (precision < 0 || raw.empty()) return raw;
    char *end = nullptr;
    const double v = std::strtod(raw.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return raw;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", precision > 6 ? 6 : precision, v);
    std::string s = buf;
    if (s == "-0" || s.rfind("-0.", 0) == 0) { /* "-0.0" reads oddly */
        bool zero = true;
        for (char c : s)
            if (c >= '1' && c <= '9') zero = false;
        if (zero) s = s.substr(1);
    }
    return s;
}

std::string format_temperature(double t, const std::string &unit)
{
    if (std::isnan(t)) return "";
    char buf[32];
    if (std::fabs(t - std::round(t)) < 0.05) std::snprintf(buf, sizeof buf, "%.0f", t);
    else std::snprintf(buf, sizeof buf, "%.1f", t);
    const std::string u = clean_unit(unit);
    return u.empty() ? std::string(buf) : std::string(buf) + " " + u;
}

bool unavailable(const State &s)
{
    return s.state.empty() || s.state == "unavailable" || s.state == "unknown";
}

bool is_on(Domain d, const State &s)
{
    switch (d) {
    case Domain::Cover: return s.state == "open" || s.state == "opening";
    case Domain::Lock: return s.state == "unlocked" || s.state == "open";
    default: return s.state == "on";
    }
}

bool confirm_cover(const State &s)
{
    return s.device_class == "garage" || s.device_class == "gate" || s.device_class == "door";
}

int brightness_pct(const State &s)
{
    if (s.state != "on" || s.brightness < 0) return 0;
    const int pct = static_cast<int>(std::lround(s.brightness * 100.0 / 255.0));
    return pct < 1 ? 1 : (pct > 100 ? 100 : pct);
}

static std::string capitalised(std::string s)
{
    for (char &c : s)
        if (c == '_') c = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

static std::string binary_word(const State &s)
{
    const bool on = s.state == "on";
    const std::string &c = s.device_class;
    if (c == "door" || c == "garage_door" || c == "opening" || c == "window") return on ? "Open" : "Closed";
    if (c == "motion" || c == "occupancy" || c == "presence" || c == "moving" || c == "vibration" || c == "sound" ||
        c == "smoke" || c == "gas" || c == "carbon_monoxide" || c == "tamper")
        return on ? "Detected" : "Clear";
    if (c == "moisture") return on ? "Wet" : "Dry";
    if (c == "problem") return on ? "Problem" : "OK";
    if (c == "safety") return on ? "Unsafe" : "Safe";
    if (c == "battery") return on ? "Low" : "Normal";
    if (c == "connectivity") return on ? "Connected" : "Disconnected";
    if (c == "lock") return on ? "Unlocked" : "Locked";
    if (c == "plug") return on ? "Plugged in" : "Unplugged";
    if (c == "running") return on ? "Running" : "Not running";
    if (c == "update") return on ? "Update available" : "Up to date";
    return on ? "On" : "Off";
}

static std::string hvac_word(const std::string &mode)
{
    if (mode == "heat_cool") return "Heat/cool";
    if (mode == "fan_only") return "Fan";
    return capitalised(mode);
}

std::string state_label(Domain d, const State &s, int precision, const std::string &temp_unit)
{
    if (s.state == "unavailable") return "Unavailable";
    if (s.state == "unknown" || s.state.empty()) return d == Domain::Scene ? "" : "Unknown";
    switch (d) {
    case Domain::Light:
    case Domain::Switch:
    case Domain::InputBoolean: return s.state == "on" ? "On" : s.state == "off" ? "Off" : capitalised(s.state);
    case Domain::Scene: return ""; /* its state is the time it last ran */
    case Domain::Script: return s.state == "on" ? "Running" : "";
    case Domain::Sensor: {
        const std::string v = format_number(s.state, precision);
        const std::string u = clean_unit(s.unit);
        return u.empty() ? v : v + " " + u;
    }
    case Domain::BinarySensor: return binary_word(s);
    case Domain::Cover:
    case Domain::Lock: return capitalised(s.state);
    case Domain::Climate: {
        std::string w = hvac_word(s.state);
        const std::string t = format_temperature(s.current_temperature, temp_unit);
        return t.empty() ? w : w + ", " + t;
    }
    default: return capitalised(s.state);
    }
}

std::string changed_text(double epoch, std::int64_t now_epoch)
{
    if (epoch <= 0) return "";
    const std::int64_t e = static_cast<std::int64_t>(epoch);
    if (now_epoch > 0 && now_epoch - e < 24 * 3600 && now_epoch >= e - 60) return clock_hhmm(e);
    const std::time_t t = static_cast<std::time_t>(e);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::vector<std::pair<std::string, std::string>> detail_rows(Domain d, const State &s, const std::string &temp_unit,
                                                             const std::string &room, std::int64_t now_epoch)
{
    std::vector<std::pair<std::string, std::string>> rows;
    switch (d) {
    case Domain::Light:
        if (s.state == "on" && s.brightness >= 0 && !s.dimmable)
            rows.emplace_back("Brightness", std::to_string(brightness_pct(s)) + " %");
        break;
    case Domain::Climate: {
        const std::string cur = format_temperature(s.current_temperature, temp_unit);
        const std::string target = format_temperature(s.target_temperature, temp_unit);
        if (!cur.empty()) rows.emplace_back("Temperature", cur);
        if (!target.empty()) rows.emplace_back("Target", target);
        if (!s.hvac_action.empty()) rows.emplace_back("Action", capitalised(s.hvac_action));
        break;
    }
    case Domain::Cover:
        if (s.current_position >= 0) rows.emplace_back("Position", std::to_string(s.current_position) + " %");
        break;
    default: break;
    }
    if (!room.empty()) rows.emplace_back("Room", room);
    const std::string changed = changed_text(s.last_changed, now_epoch);
    if (!changed.empty()) rows.emplace_back("Last changed", changed);
    return rows;
}

std::string confirm_question(const std::string &service, const std::string &name)
{
    if (service == "unlock") return "Unlock " + name + "?";
    if (service == "lock") return "Lock " + name + "?";
    if (service == "open_cover") return "Open " + name + "?";
    if (service == "close_cover") return "Close " + name + "?";
    return capitalised(service) + " " + name + "?";
}

} // namespace ha
