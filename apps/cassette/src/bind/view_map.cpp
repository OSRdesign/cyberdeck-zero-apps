/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the view model mapping (see view_map.hpp).
 */
#include "view_map.hpp"

#include "core/edit/merge.hpp"
#include "core/takes/naming.hpp"
#include "ui/ui_format.hpp"
#include "ui/wave_geom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cassette {
namespace bind {

namespace {

const char *const kDot = " \xC2\xB7 "; /* " · " */
const char *const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void join(std::string &out, const std::string &part)
{
    if (part.empty()) return;
    if (!out.empty()) out += kDot;
    out += part;
}

ui::SettingRow choice_row(const char *id, const char *label, std::vector<std::string> choices, int pick)
{
    ui::SettingRow r;
    r.id = id;
    r.label = label;
    r.kind = ui::SettingKind::Choice;
    r.choices = std::move(choices);
    r.choice = r.choices.empty() ? 0 : std::clamp(pick, 0, static_cast<int>(r.choices.size()) - 1);
    if (!r.choices.empty()) r.value = r.choices[static_cast<size_t>(r.choice)];
    return r;
}

ui::SettingRow toggle_row(const char *id, const char *label, bool on)
{
    ui::SettingRow r;
    r.id = id;
    r.label = label;
    r.kind = ui::SettingKind::Toggle;
    r.on = on;
    return r;
}

} // namespace

const int kTimerChoices[4] = {0, 3, 10, 20};
const int kSyncChoices[4] = {5, 10, 20, 30};

ui::Transport transport_of(int s)
{
    switch (s) {
    case kDeckCountdown: return ui::Transport::Countdown;
    case kDeckRecording: return ui::Transport::Recording;
    case kDeckRecordingPaused: return ui::Transport::RecordingPaused;
    case kDeckPlaying: return ui::Transport::Playing;
    case kDeckPlayingPaused: return ui::Transport::PlayingPaused;
    case kDeckScanningForward: return ui::Transport::ScanForward;
    case kDeckScanningReverse: return ui::Transport::ScanReverse;
    default: return ui::Transport::Idle;
    }
}

bool recording_family(int s) { return s == kDeckCountdown || s == kDeckRecording || s == kDeckRecordingPaused; }
bool playing_family(int s)
{
    return s == kDeckPlaying || s == kDeckPlayingPaused || s == kDeckScanningForward || s == kDeckScanningReverse;
}

std::string upper(const std::string &s)
{
    std::string o = s;
    for (char &c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
}

std::string display_name(const std::string &prefix, const std::string &label)
{
    std::string p = prefix.empty() ? std::string(kDefaultPrefix) : prefix;
    p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
    return p + " " + label;
}

std::string display_name(const std::string &file_name)
{
    TakeName tn;
    if (parse_take_name(file_name, tn)) return display_name(tn.prefix, take_label(tn));
    return file_stem(file_name);
}

std::string sticker_of(const std::string &file_name)
{
    TakeName tn;
    if (parse_take_name(file_name, tn)) return take_label(tn);
    return file_stem(file_name);
}

std::string prefix_of(const std::string &file_name)
{
    TakeName tn;
    return parse_take_name(file_name, tn) ? tn.prefix : std::string();
}

std::string time_left_text(double hours)
{
    if (!(hours > 0)) return "";
    char b[32];
    if (hours >= 100) {
        std::snprintf(b, sizeof b, "%.0f h", std::floor(hours));
        return b;
    }
    long minutes = static_cast<long>(std::floor(hours * 60.0));
    std::snprintf(b, sizeof b, "%ld h %02ld", minutes / 60, minutes % 60);
    return b;
}

std::string hours_text(double hours)
{
    if (!(hours > 0)) return "";
    char b[32];
    if (hours >= 1) std::snprintf(b, sizeof b, "%.0f h", std::floor(hours));
    else std::snprintf(b, sizeof b, "%.0f min", std::floor(hours * 60.0));
    return b;
}

std::string size_per_minute_text(const SizeEstimate &size)
{
    if (size.kind == SizeKind::Unknown || size.bytes_per_minute == 0) return "";
    const double v = static_cast<double>(size.bytes_per_minute);
    char b[48];
    /* under 1 MB: kB rounded to 5 (a figure that rounds to 1000 kB is 1.0 MB) */
    const double kb5 = std::round(v / 5000.0) * 5.0;
    if (v < 1e6 && kb5 < 1000.0) {
        std::snprintf(b, sizeof b, "%.0f kB/min", std::max(5.0, kb5));
    } else {
        const double mb = std::max(1.0, v / 1e6);
        if (std::round(mb * 10.0) / 10.0 < 10.0) std::snprintf(b, sizeof b, "%.1f MB/min", std::round(mb * 10.0) / 10.0);
        else std::snprintf(b, sizeof b, "%.0f MB/min", std::round(mb));
    }
    return (size.kind == SizeKind::About ? "about " : "") + std::string(b);
}

std::string formats_footer_text(const FormatInputs &in)
{
    if (in.state != CaptureState::Known) return "No sound input: sizes unknown";
    const AudioFormat &f = in.capture;
    char b[96];
    const std::string ch = f.channels == 1 ? "mono" : f.channels == 2 ? "stereo" : std::to_string(f.channels) + " ch";
    if (f.rate % 1000 == 0) std::snprintf(b, sizeof b, "Sizes at %u kHz ", static_cast<unsigned>(f.rate / 1000));
    else std::snprintf(b, sizeof b, "Sizes at %.1f kHz ", f.rate / 1000.0);
    return std::string(b) + ch + " " + std::to_string(f.bits) + "-bit, this card";
}

const ParamItem *selectable_param(const ParamPicker &p, int index, std::string *notice)
{
    if (index < 0 || index >= static_cast<int>(p.items.size())) return nullptr;
    const ParamItem &it = p.items[static_cast<size_t>(index)];
    if (it.header) return nullptr;
    if (it.compatible) return &it;
    if (notice) *notice = "Not available: " + it.why;
    return nullptr;
}

namespace {

/* the numbers every picker works from */
struct ParamCtx {
    const Snapshot &s;
    FormatInputs in;
    RecStored st;     /* what is stored */
    RecParams eff;    /* what records (the stored choice, else WAV) */
    bool fell_back = false;
    RecStored base;   /* the stored values a rate or quality change builds on: the effective codec when the stored one cannot record */
    bool known = false;

    explicit ParamCtx(const Snapshot &snap) : s(snap), in(snap.format_inputs), st(stored_of(snap.settings))
    {
        known = in.state == CaptureState::Known;
        eff = effective_params(in, st, &fell_back);
        base = st;
        if (fell_back) {
            base.codec = eff.codec;
            base.preset.clear();
            base.quality = 0;
        }
    }
};

const char *codec_name(RecFormat f) { return f == RecFormat::Opus ? "Opus" : f == RecFormat::Flac ? "FLAC" : "WAV"; }

std::string kbps_text(int kbps) { return std::to_string(kbps) + " kbps"; }
std::string bits_text(unsigned bits) { return std::to_string(bits) + "-bit"; }

std::string free_hours(const ParamCtx &c, const SizeEstimate &size)
{
    if (c.s.take.free_bytes == 0) return "";
    const std::string h = hours_text(hours_for_space(c.s.take.free_bytes, size));
    return h.empty() ? "" : h + " free";
}

/* line 2: the hint, then the hours that fit; a combination that cannot record: the reason */
ui::FormatEntry param_entry(const ParamCtx &c, const std::string &name, const std::string &hint, const FormatChoice &ch,
                            bool ok, const std::string &why)
{
    ui::FormatEntry e;
    e.name = name;
    e.size = size_per_minute_text(ch.size);
    e.compatible = ok;
    e.reason = ok ? std::string() : why;
    if (ok) {
        join(e.detail, hint);
        join(e.detail, free_hours(c, ch.size));
    } else {
        e.detail = why;
    }
    return e;
}

std::string rate_hint(uint32_t r)
{
    switch (r) {
    case 8000: return "telephone";
    case 11025: return "very low";
    case 12000: return "low";
    case 16000: return "speech";
    case 22050: return "low";
    case 24000: return "wide speech";
    case 32000: return "broadcast";
    case 44100: return "CD";
    case 48000: return "video and most devices";
    case 88200:
    case 96000: return "high resolution";
    case 176400:
    case 192000: return "very high resolution";
    default: return "";
    }
}

std::string kbps_hint(int kbps)
{
    switch (kbps) {
    case 16: return "speech, very small";
    case 24: return "speech, smallest";
    case 32: return "speech, clear";
    case 48: return "voice and light music";
    case 64: return "music and voice";
    case 96: return "music, good";
    case 128: return "music, best";
    default: return "";
    }
}

/* the Settings row's value text for the quality of the effective choice */
std::string quality_value(const RecParams &p) { return p.codec == RecFormat::Opus ? kbps_text(p.kbps) : bits_text(p.bits); }

} // namespace

ParamPicker codec_picker(const Snapshot &s)
{
    const ParamCtx c(s);
    ParamPicker p;
    p.label = "Codec";
    /* the value: the codec that records; a stored one that cannot says so */
    p.value = codec_name(c.eff.codec);
    if (c.fell_back) p.value += std::string(" (") + codec_name(c.st.codec) + " unavailable)";

    auto header = [&](const char *t) {
        ParamItem it;
        it.header = true;
        p.items.push_back(it);
        ui::FormatEntry e;
        e.name = t;
        e.header = true;
        p.entries.push_back(e);
    };
    header("Codecs");
    const RecFormat codecs[3] = {RecFormat::Wav, RecFormat::Flac, RecFormat::Opus};
    for (RecFormat f : codecs) {
        ParamItem it;
        it.change = change_codec(c.in, c.st, f);
        const RecParams rp = resolve_params(c.in, it.change.stored);
        const FormatChoice ch = choice_for(c.in, rp);
        it.compatible = ch.compatible;
        it.why = ch.why_not;
        if (f == c.eff.codec) p.current = static_cast<int>(p.items.size());
        p.items.push_back(it);
        p.entries.push_back(param_entry(c, codec_name(f), ch.detail, ch, ch.compatible, ch.why_not));
    }
    header("Quick presets");
    const std::vector<FormatChoice> presets = list_formats(FormatInputs()); /* the keys, names and hints of the six */
    for (const FormatChoice &pc : presets) {
        RecParams rp;
        rp.codec = pc.key.format;
        rp.rate = default_rate(c.in, rp.codec);
        rp.bits = default_bits(c.in);
        rp.kbps = pc.opus_bitrate_bps / 1000;
        const FormatChoice ch = choice_for(c.in, rp);
        std::string name = codec_name(rp.codec);
        if (rp.codec == RecFormat::Opus) name += std::string(kbps_is_speech(rp.kbps) ? " speech " : " music ") + kbps_text(rp.kbps);
        else if (rp.rate) name += ", " + rate_label(rp.rate) + ", " + bits_text(rp.bits);
        ParamItem it;
        it.preset = true;
        it.key = pc.key;
        it.compatible = ch.compatible;
        it.why = ch.why_not;
        p.items.push_back(it);
        p.entries.push_back(param_entry(c, name, pc.detail, ch, ch.compatible, ch.why_not));
    }
    if (!c.known) p.footer = "No sound input: sizes unknown";
    else p.footer = "Sizes for " + rate_label(c.eff.rate) + ", " + bits_text(c.eff.bits) + ", this card";
    return p;
}

ParamPicker rate_picker(const Snapshot &s)
{
    const ParamCtx c(s);
    ParamPicker p;
    p.label = "Sample rate";
    p.value = c.known && c.eff.rate ? rate_label(c.eff.rate) : std::string("-");
    for (uint32_t r : card_rates(c.in)) {
        std::string why;
        const bool legal = rate_legal(c.eff.codec, r, &why);
        RecParams rp = c.eff;
        rp.rate = r;
        const FormatChoice ch = choice_for(c.in, rp);
        const bool ok = legal && ch.compatible;
        ParamItem it;
        it.change = change_rate(c.base, r);
        it.compatible = ok;
        it.why = legal ? ch.why_not : why;
        if (r == c.eff.rate) p.current = static_cast<int>(p.items.size());
        p.items.push_back(it);
        p.entries.push_back(param_entry(c, rate_label(r), rate_hint(r), ch, ok, it.why));
    }
    if (!c.known) p.footer = "No sound input: sizes unknown";
    else p.footer = std::string("Sizes for ") + codec_name(c.eff.codec) + ", " +
                    (c.eff.codec == RecFormat::Opus ? kbps_text(c.eff.kbps) : bits_text(c.eff.bits)) + ", this card";
    return p;
}

ParamPicker quality_picker(const Snapshot &s)
{
    const ParamCtx c(s);
    ParamPicker p;
    const bool opus = c.eff.codec == RecFormat::Opus;
    p.label = opus ? "Bit rate" : "Bit depth";
    p.value = quality_value(c.eff);
    if (opus) {
        for (int k : kOpusKbps) {
            RecParams rp = c.eff;
            rp.kbps = k;
            const FormatChoice ch = choice_for(c.in, rp);
            ParamItem it;
            it.change = change_quality(c.base, k);
            it.compatible = ch.compatible;
            it.why = ch.why_not;
            if (k == c.eff.kbps) p.current = static_cast<int>(p.items.size());
            p.items.push_back(it);
            p.entries.push_back(param_entry(c, kbps_text(k), kbps_hint(k), ch, ch.compatible, ch.why_not));
        }
    } else {
        /* only the depths the card can record (owner: a depth the card cannot record is hidden, not dimmed); a depth
         * the codec cannot store at this rate stays, dimmed with the reason */
        for (unsigned b : card_bits(c.in)) {
            RecParams rp = c.eff;
            rp.bits = b;
            const FormatChoice ch = choice_for(c.in, rp);
            ParamItem it;
            it.change = change_quality(c.base, static_cast<int>(b));
            it.compatible = ch.compatible;
            it.why = ch.why_not;
            if (b == c.eff.bits) p.current = static_cast<int>(p.items.size());
            p.items.push_back(it);
            p.entries.push_back(param_entry(c, bits_text(b), b == 16 ? "CD quality, plays everywhere" : "more headroom, larger files",
                                            ch, ch.compatible, ch.why_not));
        }
    }
    if (!c.known) p.footer = "No sound input: sizes unknown";
    else p.footer = std::string("Sizes for ") + codec_name(c.eff.codec) + ", " + rate_label(c.eff.rate) + ", this card";
    return p;
}

std::string size_text(uint64_t bytes)
{
    const double kb = 1024.0, mb = kb * 1024, gb = mb * 1024;
    const double v = static_cast<double>(bytes);
    char b[32];
    if (v >= 10 * gb) std::snprintf(b, sizeof b, "%.0f GB", std::floor(v / gb));
    else if (v >= gb) std::snprintf(b, sizeof b, "%.1f GB", std::floor(v / gb * 10) / 10);
    else if (v >= mb) std::snprintf(b, sizeof b, "%.0f MB", std::max(1.0, std::round(v / mb)));
    else std::snprintf(b, sizeof b, "%.0f KB", std::max(1.0, std::round(v / kb)));
    return b;
}

std::string length_text(double seconds)
{
    if (!(seconds > 0)) seconds = 0;
    char b[32];
    const long s = std::lround(seconds);
    if (s < 60) std::snprintf(b, sizeof b, "%ld s", s);
    else if (s < 3600) std::snprintf(b, sizeof b, "%ld min", std::lround(seconds / 60.0));
    else std::snprintf(b, sizeof b, "%ld h %02ld", s / 3600, (s % 3600) / 60);
    return b;
}

std::string date_part(const std::string &t, int64_t now_wall_s, bool utc, bool shortest)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (t.empty() || std::sscanf(t.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3 || mo < 1 || mo > 12)
        return "nodate";
    char hm[16];
    std::snprintf(hm, sizeof hm, "%02d:%02d", h, mi);
    DateTime today, yesterday;
    if (datetime_from_wall(now_wall_s, today, utc)) {
        if (today.year == y && today.month == mo && today.day == d) return shortest ? std::string(hm) : std::string("today ") + hm;
        if (datetime_from_wall(now_wall_s - 86400, yesterday, utc) && yesterday.year == y && yesterday.month == mo &&
            yesterday.day == d)
            return std::string(shortest ? "yest " : "yesterday ") + hm;
    }
    char b[32];
    if (today.year == y) std::snprintf(b, sizeof b, "%d %s", d, kMonths[mo - 1]);
    else if (shortest) std::snprintf(b, sizeof b, "%d %s %02d", d, kMonths[mo - 1], y % 100);
    else std::snprintf(b, sizeof b, "%d %s %d", d, kMonths[mo - 1], y);
    return b;
}

std::vector<std::string> take_detail_forms(const TakeRow &r, int64_t now_wall_s, bool utc)
{
    std::vector<std::string> forms;
    if (r.damaged) {
        std::string out;
        join(out, "damaged");
        join(out, size_text(r.size));
        forms.push_back(out);
        return forms;
    }
    /* longest first: everything; then without the size; then also the date in its shortest form. The format tag
     * (WAV, FLAC, OPUS: a folder can hold all three) is in every form. */
    for (int level = 0; level < 3; ++level) {
        std::string out;
        join(out, date_part(r.date_text, now_wall_s, utc, level >= 2));
        join(out, length_text(r.seconds));
        if (level == 0) join(out, size_text(r.size));
        if (!r.format_tag.empty()) join(out, r.format_tag);
        if (r.repaired) join(out, "repaired");
        if (forms.empty() || forms.back() != out) forms.push_back(out);
    }
    return forms;
}

std::string take_detail(const TakeRow &r, int64_t now_wall_s, bool utc)
{
    return take_detail_forms(r, now_wall_s, utc).front();
}

namespace {

MergeFacts merge_facts(const TakeRow &r)
{
    MergeFacts f;
    f.named = r.named;
    f.readable = !r.damaged;
    f.kind = r.format_tag == "FLAC" ? EditKind::Flac : r.format_tag == "OPUS" ? EditKind::OggOpus : EditKind::Wav;
    f.rate = r.rate;
    f.channels = static_cast<uint16_t>(r.channels);
    f.bits = static_cast<uint16_t>(r.bits);
    return f;
}

} // namespace

std::string merge_row_reason(const Snapshot &s, const std::vector<TakeRow> &rows, const std::vector<std::string> &chosen,
                             size_t i)
{
    if (i >= rows.size()) return "";
    const TakeRow &r = rows[i];
    const int ds = s.deck.state;
    if (recording_family(ds) && ds != kDeckCountdown && s.take.name == r.name) return "Being recorded";
    if (s.trim.open && s.trim.take == r.name) return "Open on the Edit page";
    if (playing_family(ds) && (s.take.playing_name.empty() ? s.take.name : s.take.playing_name) == r.name)
        return "Playing: stop it first";
    const TakeRow *first = nullptr;
    for (const std::string &n : chosen) {
        for (const TakeRow &x : rows)
            if (x.name == n) first = &x;
        if (first) break;
    }
    if (!first || first->name == r.name) return merge_take_reason(merge_facts(r));
    return merge_join_reason(merge_facts(*first), merge_facts(r));
}

std::string merge_detail_text(double seconds, uint64_t bytes)
{
    std::string out = ui::fmt_mmss(std::llround(std::max(0.0, seconds) * 1000.0));
    join(out, "about " + size_text(bytes));
    return out;
}

std::string merge_strip_text(const std::vector<TakeRow> &rows, const std::vector<std::string> &chosen)
{
    int n = 0;
    double secs = 0;
    uint64_t bytes = 0;
    for (const std::string &c : chosen)
        for (const TakeRow &r : rows)
            if (r.name == c) {
                ++n;
                secs += r.seconds;
                bytes += r.size;
            }
    std::string out = "Merge";
    join(out, std::to_string(n) + (n == 1 ? " take" : " takes"));
    if (n > 0) join(out, merge_detail_text(secs, bytes));
    return out;
}

std::string merging_title(int percent)
{
    return "Merging... " + std::to_string(std::clamp(percent, 0, 100)) + "%";
}

std::string tape_format(const std::string &f)
{
    const size_t slash = f.find('/');
    const size_t last = f.rfind(' ');
    if (slash == std::string::npos || last == std::string::npos) return f;
    return f.substr(last + 1) + " " + f.substr(0, slash);
}

std::string short_format(const std::string &f)
{
    return f.substr(0, f.find(' '));
}

std::string format_words(const std::string &f)
{
    const size_t slash = f.find('/');
    const size_t sp = f.find(' ');
    const size_t last = f.rfind(' ');
    if (slash == std::string::npos || sp == std::string::npos || slash > sp) return f;
    std::string rate = f.substr(0, slash);
    if (!rate.empty() && rate.back() == 'k') rate.pop_back();
    const std::string bits = f.substr(slash + 1, sp - slash - 1);
    std::string out = f.substr(last + 1) + " " + rate + " kHz " + bits + "-bit";
    if (f.find(" stereo") != std::string::npos) out += " stereo";
    return out;
}

float peak_level(const PeakBar &b)
{
    const int a = std::max(std::abs(b.min), std::abs(b.max));
    return std::clamp(static_cast<float>(a) / 32767.0f, 0.0f, 1.0f);
}

std::vector<float> peak_levels(const std::vector<PeakBar> &bars)
{
    std::vector<float> out;
    out.reserve(bars.size());
    for (const PeakBar &b : bars) out.push_back(peak_level(b));
    return out;
}

int64_t frames_to_ms(uint64_t frames, uint32_t rate)
{
    if (!rate) rate = 48000;
    return static_cast<int64_t>(frames * 1000ull / rate);
}

int64_t ms_to_frames(int64_t ms, uint32_t rate)
{
    if (!rate) rate = 48000;
    return ms * static_cast<int64_t>(rate) / 1000;
}

int zoom_limit(const TrimSnap &t, double min_window_s)
{
    const uint64_t rate = t.rate ? t.rate : 48000;
    const uint64_t want = std::min<uint64_t>(t.frames, static_cast<uint64_t>(min_window_s * static_cast<double>(rate)));
    const uint64_t floor_frames = std::max<uint64_t>(1, std::min<uint64_t>(t.frames, rate / 10)); /* the controller's */
    int z = 0;
    while (z < t.max_zoom && std::max<uint64_t>(floor_frames, t.frames >> (z + 1)) >= want) ++z;
    return z;
}

std::vector<ui::SettingRow> settings_rows(const Snapshot &s, uint64_t next_take)
{
    std::vector<ui::SettingRow> rows;
    const bool busy = recording_family(s.deck.state);
    const CardSnap &c = s.card;
    {
        ui::SettingRow r = choice_row("input", "Input", c.labels, c.selected);
        if (c.labels.empty()) r.value = c.label.empty() ? "None" : c.label;
        r.enabled = !c.labels.empty() && !busy;
        rows.push_back(r);
    }
    if (c.present && c.has_level) {
        ui::SettingRow r;
        r.id = "level";
        r.label = "Input level";
        r.kind = ui::SettingKind::Slider;
        r.min = 0;
        r.max = std::max(1, static_cast<int>(std::lround(c.level_max_db - c.level_min_db)));
        r.pos = std::clamp(static_cast<int>(std::lround(c.level_db - c.level_min_db)), 0, r.max);
        r.value_text = ui::fmt_db(c.level_db, false); /* "+12 dB": the row keeps room for its label at 320 wide */
        rows.push_back(r);
    }
    if (c.present && c.has_agc) rows.push_back(toggle_row("agc", "Auto gain (card)", c.auto_gain));
    if (c.present && c.has_mute) rows.push_back(toggle_row("mute", "Mic mute (card)", c.mute));
    {
        /* Codec, Sample rate and Quality (Bit depth for WAV and FLAC, Bit rate for Opus): each opens a picker of its own */
        const bool known = s.format_inputs.state == CaptureState::Known;
        const ParamPicker cp = codec_picker(s), rp = rate_picker(s), qp = quality_picker(s);
        auto param_row = [&](const char *id, const std::string &label, const ParamPicker &pk, bool enabled) {
            ui::SettingRow r = choice_row(id, label.c_str(), {pk.value}, 0);
            r.enabled = enabled;
            r.formats = pk.entries;
            r.choice = pk.current;
            r.formats_footer = pk.footer;
            return r;
        };
        rows.push_back(param_row("codec", "Codec", cp, !busy));
        rows.push_back(param_row("rate", "Sample rate", rp, !busy && known && !rp.entries.empty()));
        /* WAV and FLAC on a card with one depth: nothing to choose, no Bit depth row (the depth is the card's) */
        const bool opus = effective_params(s.format_inputs, stored_of(s.settings)).codec == RecFormat::Opus;
        if (opus || qp.entries.size() > 1) rows.push_back(param_row("quality", qp.label, qp, !busy));
    }
    {
        int pick = 0;
        for (int i = 0; i < 4; ++i)
            if (kTimerChoices[i] == s.settings.record_timer_s) pick = i;
        rows.push_back(choice_row("timer", "Record timer", {"Off", "3 s", "10 s", "20 s"}, pick));
    }
    rows.push_back(choice_row("rewfwd", "REW / FWD", {"Fast scan 5x", "Jump 5 s"}, s.settings.rew_fwd == ScanMode::Jump ? 1 : 0));
    {
        ui::SettingRow r;
        r.id = "channels";
        r.label = "Channels";
        r.kind = ui::SettingKind::Info; /* what the card records; the controller has no channel setting: read-only */
        r.enabled = false;
        r.value = s.meter.channels == 2 ? "Stereo" : "Mono";
        rows.push_back(r);
    }
    rows.push_back(toggle_row("lowcut", "Low cut 80 Hz", s.settings.low_cut));
    rows.push_back(toggle_row("limiter", "Limiter", s.settings.limiter));
    if (c.present && c.has_playback) { /* only a card with a playback side can be monitored */
        rows.push_back(toggle_row("monitor", "Monitor", s.settings.monitor));
        ui::SettingRow r;
        r.id = "monitor_level";
        r.label = "Monitor level";
        r.kind = ui::SettingKind::Slider;
        r.enabled = s.settings.monitor; /* dimmed, not hidden, while Monitor is off */
        r.min = 0;
        r.max = 20;
        r.pos = std::clamp(s.settings.monitor_level / 5, 0, 20);
        r.value_mul = 5;
        r.value_suffix = " %";
        r.value_text = std::to_string(r.pos * 5) + " %";
        rows.push_back(r);
    }
    {
        int pick = 0; /* the nearest choice (the setting may hold any value from 5 to 30) */
        for (int i = 1; i < 4; ++i)
            if (std::abs(kSyncChoices[i] - s.settings.sync_interval_s) < std::abs(kSyncChoices[pick] - s.settings.sync_interval_s)) pick = i;
        rows.push_back(choice_row("sync", "Sync every", {"5 s", "10 s", "20 s", "30 s"}, pick));
    }
    {
        std::vector<std::string> folders = {s.settings.folder};
        if (s.settings.folder != "~/Recordings") folders.push_back("~/Recordings");
        ui::SettingRow r = choice_row("folder", "Folder", folders, 0);
        r.enabled = !busy;
        rows.push_back(r);
    }
    {
        ui::SettingRow r;
        r.id = "prefix";
        r.label = "File prefix";
        r.kind = ui::SettingKind::Text;
        r.value = s.settings.prefix;
        rows.push_back(r);
    }
    {
        /* Take number: the next number REC hands out (own block; the page opens the steppers) */
        const uint64_t n = next_take ? next_take : 1;
        ui::SettingRow r;
        r.id = "takenum";
        r.label = "Take number";
        r.kind = ui::SettingKind::Number;
        r.enabled = !busy;
        r.value = "Next: " + take_number_text(n);
        r.min = static_cast<int>(kTakeNumberMin);
        r.max = static_cast<int>(kTakeNumberMax);
        r.pos = static_cast<int>(std::min<uint64_t>(n, kTakeNumberMax)); /* the page's starting value */
        rows.push_back(r);
    }
    rows.push_back(choice_row("split", "Auto-split", {"At 4 GiB", "Off (one RF64 file)"}, s.settings.auto_split ? 0 : 1));
    if (c.output_labels.size() > 1) {
        ui::SettingRow r = choice_row("output", "Output", c.output_labels, c.output_selected);
        r.enabled = !busy;
        rows.push_back(r);
    }
    return rows;
}

ui::ViewModel map_view(const MapInput &in)
{
    static const Snapshot kEmptySnap;
    static const std::vector<TakeRow> kNoRows;
    static const LocalState kNoLocal;
    const Snapshot &s = in.snap ? *in.snap : kEmptySnap;
    const std::vector<TakeRow> &rows = in.rows ? *in.rows : kNoRows;
    const LocalState &local = in.local ? *in.local : kNoLocal;

    ui::ViewModel vm;
    const int ds = s.deck.state;
    const bool rec_side = recording_family(ds) && ds != kDeckCountdown;
    const bool play_side = playing_family(ds);
    const bool present = s.card.present;

    /* ---- recorder ---- */
    ui::RecorderVm &r = vm.rec;
    r.input_present = present;
    r.transport = transport_of(ds);
    auto key = [&](ui::KeyId id, const KeySnap &k, bool latched, bool extra_enable) {
        ui::KeyState &o = r.keys[static_cast<int>(id)];
        o.latched = latched;
        o.enabled = (k.enabled && extra_enable) || latched;
    };
    key(ui::KeyId::Rew, s.deck.rew, s.deck.rew.active, true);
    /* PLAY/PAUSE shows the current state (ui_format.hpp play_key_look): latched while a take plays, scans or is paused and
     * while a recording is paused; the glyph follows the transport in the page */
    key(ui::KeyId::PlayPause, s.deck.play_pause, ui::play_key_look(r.transport).latched, true);
    key(ui::KeyId::Fwd, s.deck.fwd, s.deck.fwd.active, true);
    key(ui::KeyId::Stop, s.deck.stop, false, true);
    key(ui::KeyId::Rec, s.deck.rec, rec_side, present);
    /* the hubs: both the same way at the same speed; a 5 s jump flashes its direction (ScanSnap::jump_dir) */
    r.hub_dps[0] = r.hub_dps[1] = ui::hub_speed_dps(r.transport, play_side ? s.scan.jump_dir : 0);
    r.scan_speed = 5;
    if (!s.scan.speed_text.empty()) r.scan_speed = std::max(1, std::atoi(s.scan.speed_text.c_str()));
    r.countdown_left = s.time.countdown_seconds;
    r.countdown_fraction = ds == kDeckCountdown ? s.time.countdown_fraction : 1.0f;
    if (recording_family(ds) && ds != kDeckCountdown) r.counter_ms = std::llround(s.time.elapsed_s * 1000.0);
    else if (play_side) r.counter_ms = std::llround(s.time.position_s * 1000.0);
    if (play_side) {
        /* a take plays, scans or is paused: the bars show the PLAYED level (Snapshot::play_meter) on L and R (a mono take
         * drives both equally); the PEAK lamp is the recording's clip warning and stays off */
        const MeterSnap &pm = s.play_meter;
        r.meter.channels = 2;
        for (int i = 0; i < 2; ++i) {
            const MeterChannelSnap &c = pm.ch[pm.channels == 2 ? i : 0];
            r.meter.ch[i].bar = pm.channels > 0 ? c.bar : 0;
            r.meter.ch[i].hold_bar = pm.channels > 0 ? c.hold_bar : 0;
            r.meter.ch[i].peak_lamp = false;
        }
    } else {
        /* idle, countdown, recording: the live INPUT level */
        r.meter.channels = s.meter.channels == 2 ? 2 : 1;
        if (present)
            for (int i = 0; i < 2; ++i) {
                r.meter.ch[i].bar = s.meter.ch[i].bar;
                r.meter.ch[i].hold_bar = s.meter.ch[i].hold_bar;
                r.meter.ch[i].peak_lamp = s.meter.ch[i].peak_lamp;
            }
    }
    /* the MON lamp: where the Monitor rows exist; lit by the monitor's REAL running state, not by the setting */
    r.monitor_shown = present && s.card.has_playback;
    r.monitor_active = r.monitor_shown && s.card.monitor_active;

    /* the take the cassette and the strip name: the take being recorded, the take playing (a list preview may play another
     * one than the loaded), else the LOADED take; with nothing loaded the strip names the take REC would write next and the
     * window shows the blank cassette */
    std::string take_name;
    std::string tape_fmt = s.take.format_text;
    const bool have_loaded = !s.take.name.empty();
    if (rec_side && have_loaded) {
        take_name = display_name(s.take.name);
    } else if (play_side && (have_loaded || !s.take.playing_name.empty())) {
        const std::string &pn = s.take.playing_name.empty() ? s.take.name : s.take.playing_name;
        take_name = display_name(pn);
        for (const TakeRow &t : rows)
            if (t.name == pn && !t.format_text.empty()) tape_fmt = t.format_text;
    } else if (have_loaded) {
        take_name = display_name(s.take.name);
        for (const TakeRow &t : rows)
            if (t.name == s.take.name && !t.format_text.empty()) tape_fmt = t.format_text;
    } else {
        TakeName tn;
        tn.prefix = valid_prefix(s.settings.prefix) ? s.settings.prefix : std::string(kDefaultPrefix);
        tn.take = local.next_take ? local.next_take : 1;
        take_name = display_name(tn.prefix, take_label(tn));
    }
    const bool in_deck = have_loaded || (play_side && !s.take.playing_name.empty()); /* a cassette in the window */
    r.tape.loaded = present; /* the cassette is always in the deck; with no take it is blank (no name on the label) */
    r.tape.label = in_deck ? take_name : std::string();
    r.tape.load_seq = s.take.load_seq;
    r.tape.format = tape_format(tape_fmt);
    r.info.take = present || rec_side || play_side ? upper(take_name) : "NO INPUT";
    r.info.format = present ? s.take.format_text : std::string();
    if (present) {
        /* the box's codec line: the take being recorded, idle the Settings choice (both are the controller's format_text) */
        r.info.codec = tape_format(s.take.format_text);
        r.info.kbps = s.take.opus_kbps;
    } else {
        /* no input: the codec of the Settings choice, with the rate when one is stored or known */
        const RecParams p = resolve_params(s.format_inputs, stored_of(s.settings));
        std::string c = upper(codec_name(p.codec));
        if (p.rate) {
            char b[24];
            std::snprintf(b, sizeof b, " %gk", p.rate / 1000.0);
            c += b;
        }
        r.info.codec = c;
        r.info.kbps = p.codec == RecFormat::Opus ? p.kbps : 0;
    }
    r.info.time_left = time_left_text(s.take.hours_left);
    if (play_side) {
        r.info.play_pos_ms = std::llround(s.time.position_s * 1000.0);
        r.info.play_len_ms = std::llround(s.time.length_s * 1000.0);
    }
    r.info.server = present && s.card.via_server;
    if (r.info.server && ds == kDeckIdle) r.info.card = upper(s.card.label);
    if (rec_side && s.problems.frames_lost > 0)
        r.info.lost_ms = std::max<long long>(1, std::llround(s.problems.lost_seconds * 1000.0));
    r.info.limiter = s.settings.limiter;
    r.info.low_cut = s.settings.low_cut;
    vm.notice = local.notice;
    /* the standing reason why there is no input stays in the strip until a card is found */
    if (!present && !s.card.labels.empty()) r.info.notice = s.card.message;

    r.card.shown = present && s.card.has_level;
    r.card.db = s.card.level_db;
    r.card.can_down = r.card.shown && s.card.level_db > s.card.level_min_db + 0.01;
    r.card.can_up = r.card.shown && s.card.level_db < s.card.level_max_db - 0.01;
    r.soft.shown = true;
    r.soft.db = s.card.soft_gain_db;
    r.soft.can_down = s.card.soft_gain_db > kSoftGainMin + 0.01;
    r.soft.can_up = s.card.soft_gain_db < kSoftGainMax - 0.01;

    /* ---- settings ---- */
    vm.settings.rows = settings_rows(s, local.next_take);

    /* ---- list ---- */
    ui::ListVm &l = vm.list;
    int current = -1, chosen = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        const TakeRow &t = rows[i];
        ui::TakeRow o;
        o.id = t.name;
        o.sticker = t.label.empty() ? sticker_of(t.name) : t.label;
        o.name = display_name(t.name);
        {
            std::vector<std::string> forms = take_detail_forms(t, in.now_wall_s, in.utc);
            o.detail = forms.front();
            o.detail_short.assign(forms.begin() + 1, forms.end());
        }
        o.length_ms = std::llround(t.seconds * 1000.0);
        o.prefix = prefix_of(t.name);
        l.takes.push_back(o);
        if (t.current && current < 0) current = static_cast<int>(i);
        if (!local.list_sel.empty() && t.name == local.list_sel) chosen = static_cast<int>(i);
    }
    l.selected = chosen >= 0 ? chosen : current >= 0 ? current : (rows.empty() ? -1 : 0);
    l.can_load = !recording_family(ds);
    if (play_side && !s.take.playing_name.empty())
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].name == s.take.playing_name) l.previewing = static_cast<int>(i);
    l.take_count = static_cast<int>(rows.size());
    l.folder = s.settings.folder;
    l.free_space = s.take.free_bytes ? size_text(s.take.free_bytes) : std::string();
    l.free_hours = hours_text(s.take.hours_left);
    l.format_short = short_format(s.take.format_text);
    /* merge takes: MERGE needs two takes; in merge mode each row's number in the order chosen and whether it can join */
    l.can_merge = rows.size() >= 2;
    if (local.merge_mode) {
        l.merge_mode = true;
        std::vector<std::string> chosen;
        for (const std::string &n : local.merge_sel)
            for (const TakeRow &t : rows)
                if (t.name == n) chosen.push_back(n);
        for (size_t i = 0; i < rows.size(); ++i) {
            ui::TakeRow &o = l.takes[i];
            const auto it = std::find(chosen.begin(), chosen.end(), rows[i].name);
            o.merge_order = it == chosen.end() ? 0 : static_cast<int>(it - chosen.begin()) + 1;
            o.merge_reason = o.merge_order ? std::string() : merge_row_reason(s, rows, chosen, i);
            o.merge_ok = o.merge_reason.empty();
        }
        l.merge_strip = merge_strip_text(rows, chosen);
        l.merge_ready = chosen.size() >= 2;
    }

    /* ---- trim ---- */
    if (s.trim.open) {
        const TrimSnap &t = s.trim;
        ui::TrimVm &o = vm.trim;
        o.take_id = t.take;
        o.sticker = sticker_of(t.take);
        o.take = upper(display_name(t.take));
        o.length_ms = frames_to_ms(t.frames, t.rate);
        o.marker_ms = frames_to_ms(in.trim_position >= 0 ? static_cast<uint64_t>(in.trim_position) : t.position, t.rate);
        o.span_ms = std::max<int64_t>(1, frames_to_ms(t.visible_frames, t.rate));
        o.in_ms = frames_to_ms(t.in_frame, t.rate);
        o.out_ms = frames_to_ms(t.out_frame, t.rate);
        for (uint64_t sp : t.splits) o.splits.push_back(frames_to_ms(sp, t.rate));
        const int sel = t.selected_split;
        o.selected_split = local.tap_selected && sel >= 0 && sel < static_cast<int>(t.splits.size()) &&
                                   t.splits[static_cast<size_t>(sel)] == t.position
                               ? sel
                               : -1;
        if (in.window) {
            o.window_peaks = peak_levels(*in.window);
            if (in.window_fpb > 0 && !in.window->empty()) {
                o.bar_first = in.window_first;
                o.bar_frames = in.window_fpb;
                o.rate = t.rate ? t.rate : 48000;
            }
        }
        if (in.overview) o.overview_peaks = peak_levels(*in.overview);
        /* the whole take's loudest peak = the loudest bar of the overview (it covers the take; each bar keeps its maximum) */
        float take_peak = 0.0f;
        for (float v : o.overview_peaks) take_peak = std::max(take_peak, v);
        o.fit = s.settings.wave_fit;
        o.fit_gain = ui::fit_gain_for(take_peak);
        o.playing = t.previewing;
    }

    /* ---- boxes ---- */
    vm.save.shown = local.save_shown;
    vm.save.files = local.save_files;
    if (local.confirm == LocalState::Confirm::Replace) {
        vm.confirm.shown = true;
        vm.confirm.title = "Delete the original take?";
        vm.confirm.body = "It is deleted only after every new file is written and synced.";
        vm.confirm.yes = "DELETE";
    } else if (local.confirm == LocalState::Confirm::Delete) {
        vm.confirm.shown = true;
        vm.confirm.title = "Delete this take?";
        vm.confirm.body = local.confirm_take;
        vm.confirm.yes = "DELETE";
    } else if (local.confirm == LocalState::Confirm::MergeDelete) {
        vm.confirm.shown = true;
        vm.confirm.title = "Delete the originals?";
        vm.confirm.body = "They are deleted only after the new file is written and synced.";
        vm.confirm.yes = "DELETE";
    }
    if (local.merge_box) {
        vm.merge_box.shown = true;
        vm.merge_box.title = "Merge " + std::to_string(local.merge_info.count) + " takes?";
        vm.merge_box.name = local.merge_info.name;
        vm.merge_box.detail = merge_detail_text(local.merge_info.seconds, local.merge_info.bytes);
    }
    if (local.merge_running) {
        vm.progress.shown = true;
        vm.progress.title = merging_title(local.merge_shown_pct);
        vm.progress.body = std::to_string(local.merge_count) + " takes into " + local.merge_name;
    }
    return vm;
}

bool same(const ui::RecorderVm &a, const ui::RecorderVm &b)
{
    if (a.input_present != b.input_present || a.transport != b.transport || a.scan_speed != b.scan_speed ||
        a.countdown_left != b.countdown_left || a.countdown_fraction != b.countdown_fraction || a.counter_ms != b.counter_ms ||
        a.hub_dps[0] != b.hub_dps[0] || a.hub_dps[1] != b.hub_dps[1])
        return false;
    for (int i = 0; i < static_cast<int>(ui::KeyId::kCount); ++i)
        if (a.keys[i].enabled != b.keys[i].enabled || a.keys[i].latched != b.keys[i].latched) return false;
    if (a.meter.channels != b.meter.channels) return false;
    for (int i = 0; i < 2; ++i)
        if (a.meter.ch[i].bar != b.meter.ch[i].bar || a.meter.ch[i].hold_bar != b.meter.ch[i].hold_bar ||
            a.meter.ch[i].peak_lamp != b.meter.ch[i].peak_lamp)
            return false;
    if (a.monitor_shown != b.monitor_shown || a.monitor_active != b.monitor_active) return false;
    if (a.tape.loaded != b.tape.loaded || a.tape.label != b.tape.label ||
        a.tape.format != b.tape.format || a.tape.load_seq != b.tape.load_seq)
        return false;
    const ui::InfoVm &x = a.info, &y = b.info;
    if (x.take != y.take || x.format != y.format || x.codec != y.codec || x.kbps != y.kbps || x.time_left != y.time_left || x.play_pos_ms != y.play_pos_ms ||
        x.play_len_ms != y.play_len_ms || x.server != y.server || x.card != y.card || x.lost_ms != y.lost_ms || x.limiter != y.limiter || x.low_cut != y.low_cut || x.notice != y.notice)
        return false;
    auto gain = [](const ui::GainVm &g, const ui::GainVm &h) {
        return g.shown == h.shown && g.db == h.db && g.can_down == h.can_down && g.can_up == h.can_up;
    };
    return gain(a.card, b.card) && gain(a.soft, b.soft);
}

bool same(const ui::ViewModel &a, const ui::ViewModel &b)
{
    return a.notice == b.notice && same(a.rec, b.rec) && a.settings == b.settings && a.list == b.list && a.trim == b.trim && a.save == b.save &&
           a.confirm == b.confirm && a.merge_box == b.merge_box && a.progress == b.progress;
}

} // namespace bind
} // namespace cassette
