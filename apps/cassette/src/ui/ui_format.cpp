/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the texts of the screens (see ui_format.hpp).
 */
#include "ui_format.hpp"

#include <cmath>
#include <cstdio>

namespace cassette {
namespace ui {

namespace {

const char *const kDot = " \xC2\xB7 "; /* " · " */

std::string hms(std::int64_t total_s, bool pad_minutes)
{
    char b[32];
    const long long h = total_s / 3600, m = (total_s / 60) % 60, s = total_s % 60;
    if (h > 0) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", h, m, s);
    else if (pad_minutes) std::snprintf(b, sizeof b, "%02lld:%02lld", m, s);
    else std::snprintf(b, sizeof b, "%lld:%02lld", m, s);
    return b;
}

void join(std::string &out, const std::string &part)
{
    if (part.empty()) return;
    if (!out.empty()) out += kDot;
    out += part;
}

bool playing(Transport t)
{
    return t == Transport::Playing || t == Transport::PlayingPaused || t == Transport::ScanForward ||
           t == Transport::ScanReverse;
}

std::string pos_len(const RecorderVm &r)
{
    return fmt_mmss(r.info.play_pos_ms) + " / " + fmt_mmss(r.info.play_len_ms);
}

} // namespace

std::string fmt_counter(std::int64_t ms)
{
    return hms(ms < 0 ? 0 : ms / 1000, true);
}

bool counter_long(std::int64_t ms)
{
    return ms >= 3600000;
}

std::string fmt_mmss(std::int64_t ms)
{
    return hms(ms < 0 ? 0 : ms / 1000, true);
}

std::string fmt_tenths(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const std::int64_t tenths = ms / 100;
    char b[40];
    std::snprintf(b, sizeof b, "%s.%d", hms(tenths / 10, true).c_str(), static_cast<int>(tenths % 10));
    return b;
}

std::string fmt_tenths_short(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    const std::int64_t tenths = ms / 100;
    char b[40];
    std::snprintf(b, sizeof b, "%s.%d", hms(tenths / 10, false).c_str(), static_cast<int>(tenths % 10));
    return b;
}

std::string fmt_short(std::int64_t ms)
{
    if (ms < 0) ms = 0;
    return hms((ms + 500) / 1000, false);
}

std::string fmt_ruler(std::int64_t ms)
{
    return hms(ms < 0 ? 0 : ms / 1000, false);
}

std::string fmt_zoom(std::int64_t span_ms)
{
    if (span_ms < 0) span_ms = 0;
    char b[32];
    if (span_ms < 10000 && span_ms % 1000 != 0)
        std::snprintf(b, sizeof b, "%.1f s", static_cast<double>(span_ms) / 1000.0);
    else if (span_ms < 100000)
        std::snprintf(b, sizeof b, "%lld s", static_cast<long long>((span_ms + 500) / 1000));
    else
        std::snprintf(b, sizeof b, "%lld min", static_cast<long long>((span_ms + 30000) / 60000));
    return b;
}

std::string fmt_db(double db, bool decimal)
{
    char b[32];
    const double r = decimal ? std::round(db * 10.0) / 10.0 : std::round(db);
    if (r == 0.0) return decimal ? "0.0 dB" : "0 dB";
    if (decimal) std::snprintf(b, sizeof b, "%+.1f dB", r);
    else std::snprintf(b, sizeof b, "%+.0f dB", r);
    return b;
}

int bar_steps(float db)
{
    /* the same float expression as core Meter::bar_steps, so both agree at every step edge */
    int n = 0;
    for (int i = 0; i < 15; ++i)
        if (db >= -40.0f + float(i) * (40.0f / float(15 - 1))) n = i + 1;
    return n;
}

LedBand led_band(int step)
{
    if (step < 10) return LedBand::Green;
    if (step < 13) return LedBand::Amber;
    return LedBand::Red;
}

std::string info_line(const RecorderVm &r)
{
    if (!r.info.notice.empty()) return r.info.notice;
    std::string out;
    if (playing(r.transport)) {
        out = r.info.take.empty() ? std::string("PLAYING") : "PLAYING " + r.info.take;
        join(out, pos_len(r));
        if (r.transport == Transport::ScanReverse) join(out, "REW " + std::to_string(r.scan_speed) + "x");
        if (r.transport == Transport::ScanForward) join(out, "FWD " + std::to_string(r.scan_speed) + "x");
        return out;
    }
    join(out, r.info.take);
    join(out, r.info.format);
    if (!r.info.time_left.empty()) join(out, r.info.time_left + " left");
    return out;
}

std::string info_title(const RecorderVm &r)
{
    return r.info.take;
}

std::string info_detail(const RecorderVm &r)
{
    if (!r.info.notice.empty()) return r.info.notice;
    if (playing(r.transport)) return pos_len(r);
    return r.info.time_left.empty() ? std::string() : r.info.time_left + " left";
}

std::string info_state(const RecorderVm &r)
{
    switch (r.transport) {
    case Transport::Recording:
    case Transport::RecordingPaused:
    case Transport::Countdown: return "REC";
    case Transport::Playing:
    case Transport::ScanForward:
    case Transport::ScanReverse: return "PLAY";
    default: return "";
    }
}

std::string list_strip_left(const ListVm &l, bool narrow)
{
    std::string out = std::to_string(l.take_count) + (l.take_count == 1 ? " TAKE" : " TAKES");
    if (!narrow) join(out, l.folder);
    return out;
}

std::string list_strip_right(const ListVm &l, bool narrow)
{
    std::string out;
    if (!l.free_space.empty()) out = l.free_space + " FREE";
    if (!l.free_hours.empty()) {
        std::string h = l.free_hours;
        if (!narrow && !l.format_short.empty()) h += " at " + l.format_short;
        join(out, h);
    }
    return out;
}

std::string list_hint(const ListVm &l)
{
    if (l.selected < 0 || l.selected >= static_cast<int>(l.takes.size())) return "Esc returns to the recorder.";
    return l.takes[static_cast<std::size_t>(l.selected)].name + " selected. Esc returns to the recorder.";
}

std::string splits_text(std::size_t n)
{
    if (n == 0) return "no splits";
    if (n == 1) return "1 split";
    return std::to_string(n) + " splits";
}

std::string trim_strip_wide(const TrimVm &t)
{
    std::string out = "IN " + fmt_tenths(t.in_ms);
    join(out, "OUT " + fmt_tenths(t.out_ms));
    join(out, "LEN " + fmt_tenths(t.out_ms - t.in_ms));
    join(out, splits_text(t.splits.size()));
    return out;
}

std::string trim_strip_narrow_top(const TrimVm &t)
{
    std::string out = splits_text(t.splits.size());
    join(out, "LEN " + fmt_short(t.out_ms - t.in_ms));
    return out;
}

std::string trim_strip_narrow_left(const TrimVm &t)
{
    std::string out = "IN " + fmt_short(t.in_ms);
    join(out, "OUT " + fmt_short(t.out_ms));
    return out;
}

std::string trim_strip_narrow_right(const TrimVm &t)
{
    if (t.selected_split < 0 || t.selected_split >= static_cast<int>(t.splits.size())) return "";
    return "SPLIT " + std::to_string(t.selected_split + 1) + " SELECTED";
}

std::string save_summary(const TrimVm &t, std::size_t files, bool narrow)
{
    const std::string a = narrow ? fmt_short(t.in_ms) : fmt_tenths(t.in_ms);
    const std::string b = narrow ? fmt_short(t.out_ms) : fmt_tenths(t.out_ms);
    std::string out = "Keep " + a + " to " + b;
    if (!t.splits.empty()) out += ", cut at " + splits_text(t.splits.size());
    out += ": " + std::to_string(files) + (files == 1 ? " file." : " files.");
    return out;
}

std::string strip_wav(const std::string &name)
{
    const std::string ext = ".wav";
    if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
        return name.substr(0, name.size() - ext.size());
    return name;
}

std::string more_below(const std::vector<std::string> &labels)
{
    if (labels.empty()) return "";
    std::string out = "more below: ";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i) out += ", ";
        out += labels[i];
    }
    return out;
}

std::string more_below_fit(const std::vector<std::string> &labels, const std::function<bool(const std::string &)> &fits)
{
    const std::string all = more_below(labels);
    if (all.empty() || !fits || fits(all)) return all;
    std::string out = "more below: ";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const std::string next = out + (i ? ", " : "") + labels[i];
        if (!fits(next + ", ...")) break;
        out = next;
    }
    return out + (out == "more below: " ? "..." : ", ...");
}

} // namespace ui
} // namespace cassette
