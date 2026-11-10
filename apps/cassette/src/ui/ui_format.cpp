/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the texts of the screens (see ui_format.hpp).
 */
#include "ui_format.hpp"

#include "theme.hpp"

#include <algorithm>
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

bool recording(Transport t)
{
    return t == Transport::Recording || t == Transport::RecordingPaused;
}

std::string pos_len(const RecorderVm &r)
{
    return fmt_mmss(r.info.play_pos_ms) + " / " + fmt_mmss(r.info.play_len_ms);
}

} // namespace

PlayKeyLook play_key_look(Transport t)
{
    switch (t) {
    case Transport::Playing:
    case Transport::ScanForward:
    case Transport::ScanReverse: return {false, true};
    case Transport::PlayingPaused: return {true, true};
    case Transport::Recording: return {true, false};
    case Transport::RecordingPaused: return {true, true};
    case Transport::Idle:
    case Transport::Countdown: break;
    }
    return {false, false};
}

double hub_speed_dps(Transport t, int jump_dir)
{
    if (jump_dir > 0) return 3 * kHubPlayDps;
    if (jump_dir < 0) return -3 * kHubPlayDps;
    switch (t) {
    case Transport::Recording:
    case Transport::Playing: return kHubPlayDps;
    case Transport::ScanForward: return 3 * kHubPlayDps;
    case Transport::ScanReverse: return -3 * kHubPlayDps;
    default: return 0.0;
    }
}

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
        const char *verb = r.transport == Transport::PlayingPaused ? "PAUSED" : "PLAYING";
        out = r.info.take.empty() ? std::string(verb) : std::string(verb) + " " + r.info.take;
        join(out, pos_len(r));
        if (r.transport == Transport::ScanReverse) join(out, "REW " + std::to_string(r.scan_speed) + "x");
        if (r.transport == Transport::ScanForward) join(out, "FWD " + std::to_string(r.scan_speed) + "x");
        return out;
    }
    const bool rec = recording(r.transport);
    if (r.transport == Transport::Idle && r.info.server && !r.info.card.empty()) join(out, r.info.card + " (server)");
    join(out, r.info.take);
    const std::string lost = info_lost(r);
    const bool card_first = r.transport == Transport::Idle && r.info.server && !r.info.card.empty();
    /* the strip is one short line: the card name takes the format's place, the lost audio the time left's */
    if (!card_first) join(out, r.info.format.empty() || !rec || !r.info.server ? r.info.format : r.info.format + " (server)");
    if (lost.empty() && !r.info.time_left.empty()) join(out, r.info.time_left + " left");
    join(out, lost);
    return out;
}

std::string info_codec(const RecorderVm &r)
{
    if (playing(r.transport) && !r.tape.format.empty()) return r.tape.format;
    std::string out = r.info.codec;
    if (r.info.kbps > 0) join(out, std::to_string(r.info.kbps) + " kbps");
    return out;
}

int largest_fit_px(int max_px, int min_px, int max_w, const std::function<int(int)> &width_at)
{
    for (int p = max_px; p > min_px; --p)
        if (width_at(p) <= max_w) return p;
    return min_px < max_px ? min_px : max_px;
}

namespace {

/* the longest prefix of s (cut at a UTF-8 character boundary) that fits, at least one character */
std::size_t prefix_fitting(const std::string &s, int max_w, const std::function<int(const std::string &)> &width)
{
    std::size_t best = 0;
    for (std::size_t i = 1; i <= s.size(); ++i) {
        if (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (width(s.substr(0, i)) > max_w) break;
        best = i;
    }
    if (best == 0) { /* not even one character fits: take it anyway */
        best = 1;
        while (best < s.size() && (static_cast<unsigned char>(s[best]) & 0xC0) == 0x80) ++best;
    }
    return best;
}

/* s cut (at a character boundary) so that s + "..." fits */
std::string with_dots(const std::string &s, int max_w, const std::function<int(const std::string &)> &width)
{
    std::string t = s;
    while (!t.empty()) {
        if (width(t + "...") <= max_w) return t + "...";
        std::size_t n = t.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) --n;
        t.erase(n);
    }
    return "...";
}

} // namespace

NameWrap wrap_name(const std::string &name, int max_w, int max_lines, const std::function<int(const std::string &)> &width)
{
    NameWrap w;
    if (name.empty() || width(name) <= max_w || max_lines < 2) {
        w.lines.push_back(name);
        if (width(name) > max_w) {
            w.fits = false;
            w.lines[0] = with_dots(name, max_w, width);
        }
        return w;
    }
    /* two lines: the space whose split leaves the shorter longer line */
    int best = -1, best_w = 0;
    for (std::size_t i = 1; i + 1 < name.size(); ++i) {
        if (name[i] != ' ') continue;
        std::size_t a = i, b = i + 1;
        while (a > 0 && name[a - 1] == ' ') --a;
        while (b < name.size() && name[b] == ' ') ++b;
        if (a == 0 || b >= name.size()) continue;
        const int w1 = width(name.substr(0, a)), w2 = width(name.substr(b));
        if (w1 > max_w || w2 > max_w) continue;
        const int m = std::max(w1, w2);
        if (best < 0 || m < best_w) best = static_cast<int>(i), best_w = m;
    }
    if (best >= 0) {
        std::size_t a = static_cast<std::size_t>(best), b = a + 1;
        while (a > 0 && name[a - 1] == ' ') --a;
        while (b < name.size() && name[b] == ' ') ++b;
        w.lines = {name.substr(0, a), name.substr(b)};
        return w;
    }
    /* no space split fits: the first line takes what fits (up to a space when there is one), the rest is the second */
    std::size_t cut = prefix_fitting(name, max_w, width);
    const std::size_t sp = name.rfind(' ', cut);
    if (sp != std::string::npos && sp > 0 && cut < name.size()) cut = sp;
    std::string l1 = name.substr(0, cut), l2 = name.substr(cut);
    while (!l2.empty() && l2[0] == ' ') l2.erase(0, 1);
    while (!l1.empty() && l1.back() == ' ') l1.pop_back();
    w.lines = {l1, l2};
    if (width(l2) > max_w) {
        w.fits = false;
        w.lines[1] = with_dots(l2, max_w, width);
    }
    return w;
}

NameLayout layout_name_text(const std::string &name, const std::vector<int> &rungs, int wanted, int max_w, int max_h,
                            const std::function<int(int, const std::string &)> &width, const std::function<int(int)> &line_h)
{
    NameLayout best;
    for (std::size_t i = 0; i < rungs.size(); ++i) {
        const int r = rungs[i];
        if (r > wanted && i + 1 < rungs.size()) continue;
        NameLayout l;
        l.rung = r;
        const int lh = line_h(r);
        l.pitch = std::min(lh, r + 1);
        l.wrap = wrap_name(name, max_w, 2, [&](const std::string &s) { return width(r, s); });
        l.text_h = (static_cast<int>(l.wrap.lines.size()) - 1) * l.pitch + lh;
        best = l;
        if (l.wrap.fits && l.text_h <= max_h) return l;
    }
    return best;
}

std::string info_detail(const RecorderVm &r)
{
    if (!r.info.notice.empty()) return r.info.notice;
    if (playing(r.transport)) return pos_len(r);
    const std::string lost = info_lost(r);
    if (!lost.empty()) return lost;
    if (r.info.server && (recording(r.transport) || r.transport == Transport::Idle))
        return r.info.time_left.empty() ? std::string("(server)") : "(server) " + r.info.time_left;
    return r.info.time_left.empty() ? std::string() : r.info.time_left + " left";
}

std::string info_lost(const RecorderVm &r)
{
    if (!recording(r.transport) || r.info.lost_ms <= 0) return std::string();
    if (r.info.lost_ms < 50) return "lost <0.1 s";
    char b[32];
    std::snprintf(b, sizeof b, "lost %.1f s", static_cast<double>(r.info.lost_ms) / 1000.0);
    return b;
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

ListRowGeom list_row_geom(int std_h, int name_box1, int detail_box, int name_text_h, int name_line_h)
{
    ListRowGeom g;
    const bool multi = name_text_h > name_line_h;
    g.name_box = multi ? name_text_h : name_box1;
    g.h = std_h + (g.name_box - name_box1);
    const int top = (g.h - 2 - (g.name_box + detail_box)) / 2; /* inside the 1 px border, as the one-line row centres it */
    g.name_y = top + (g.name_box - (multi ? name_text_h : name_line_h)) / 2;
    g.detail_y = top + g.name_box;
    return g;
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

std::string name_count_text(std::size_t used, std::size_t limit)
{
    return std::to_string(used) + "/" + std::to_string(limit);
}

CountTone name_count_tone(std::size_t used, std::size_t limit)
{
    if (used >= limit) return CountTone::Red;
    if (used + 4 >= limit) return CountTone::Amber; /* the last four before full: 20..23 of 24 */
    return CountTone::Muted;
}

std::uint32_t name_count_color(CountTone t)
{
    return t == CountTone::Red ? col::kRingRed : t == CountTone::Amber ? col::kAmber : col::kHint;
}

} // namespace ui
} // namespace cassette
