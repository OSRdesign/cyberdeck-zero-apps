/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the view model mapping (see view_map.hpp).
 */
#include "view_map.hpp"

#include "core/takes/naming.hpp"
#include "ui/ui_format.hpp"

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

std::string date_part(const std::string &t, int64_t now_wall_s, bool utc)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (t.empty() || std::sscanf(t.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3 || mo < 1 || mo > 12)
        return "nodate";
    char hm[16];
    std::snprintf(hm, sizeof hm, "%02d:%02d", h, mi);
    DateTime today, yesterday;
    if (datetime_from_wall(now_wall_s, today, utc)) {
        if (today.year == y && today.month == mo && today.day == d) return std::string("today ") + hm;
        if (datetime_from_wall(now_wall_s - 86400, yesterday, utc) && yesterday.year == y && yesterday.month == mo &&
            yesterday.day == d)
            return std::string("yesterday ") + hm;
    }
    char b[32];
    if (today.year == y) std::snprintf(b, sizeof b, "%d %s", d, kMonths[mo - 1]);
    else std::snprintf(b, sizeof b, "%d %s %d", d, kMonths[mo - 1], y);
    return b;
}

std::string take_detail(const TakeRow &r, int64_t now_wall_s, bool utc)
{
    std::string out;
    if (r.damaged) {
        join(out, "damaged");
        join(out, size_text(r.size));
        return out;
    }
    join(out, date_part(r.date_text, now_wall_s, utc));
    join(out, length_text(r.seconds));
    join(out, size_text(r.size));
    if (r.repaired) join(out, "repaired");
    return out;
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

std::vector<ui::SettingRow> settings_rows(const Snapshot &s)
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
        r.value_text = ui::fmt_db(c.level_db, true);
        rows.push_back(r);
    }
    if (c.present && c.has_agc) rows.push_back(toggle_row("agc", "Auto gain (card)", c.auto_gain));
    {
        ui::SettingRow r = choice_row("format", "Format", {format_words(s.take.format_text)}, 0);
        r.enabled = !busy;
        rows.push_back(r);
    }
    {
        int pick = 0;
        for (int i = 0; i < 4; ++i)
            if (kTimerChoices[i] == s.settings.record_timer_s) pick = i;
        rows.push_back(choice_row("timer", "Record timer", {"Off", "3 s", "10 s", "20 s"}, pick));
    }
    rows.push_back(choice_row("rewfwd", "REW / FWD", {"Fast scan 5x", "Jump 5 s"}, s.settings.rew_fwd == ScanMode::Jump ? 1 : 0));
    {
        ui::SettingRow r = choice_row("channels", "Channels", {"Mono", "Stereo"}, s.meter.channels == 2 ? 1 : 0);
        r.enabled = false; /* what the card records; the controller has no channel setting */
        rows.push_back(r);
    }
    rows.push_back(toggle_row("lowcut", "Low cut 80 Hz", s.settings.low_cut));
    rows.push_back(toggle_row("limiter", "Limiter", s.settings.limiter));
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
    rows.push_back(choice_row("split", "Auto-split", {"At 4 GiB", "Off (one RF64 file)"}, s.settings.auto_split ? 0 : 1));
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
    key(ui::KeyId::PlayPause, s.deck.play_pause, play_side || ds == kDeckRecordingPaused, true);
    key(ui::KeyId::Fwd, s.deck.fwd, s.deck.fwd.active, true);
    key(ui::KeyId::Stop, s.deck.stop, false, true);
    key(ui::KeyId::Rec, s.deck.rec, rec_side, present);
    r.scan_speed = 5;
    if (!s.scan.speed_text.empty()) r.scan_speed = std::max(1, std::atoi(s.scan.speed_text.c_str()));
    r.countdown_left = s.time.countdown_seconds;
    r.countdown_fraction = ds == kDeckCountdown ? s.time.countdown_fraction : 1.0f;
    if (recording_family(ds) && ds != kDeckCountdown) r.counter_ms = std::llround(s.time.elapsed_s * 1000.0);
    else if (play_side) r.counter_ms = std::llround(s.time.position_s * 1000.0);
    r.meter.channels = s.meter.channels == 2 ? 2 : 1;
    if (present)
        for (int i = 0; i < 2; ++i) {
            r.meter.ch[i].bar = s.meter.ch[i].bar;
            r.meter.ch[i].hold_bar = s.meter.ch[i].hold_bar;
            r.meter.ch[i].peak_lamp = s.meter.ch[i].peak_lamp;
        }

    /* the take the cassette and the strip name */
    std::string take_name;
    if ((rec_side || play_side) && !s.take.name.empty()) {
        take_name = display_name(s.take.name);
    } else {
        TakeName tn;
        tn.prefix = valid_prefix(s.settings.prefix) ? s.settings.prefix : std::string(kDefaultPrefix);
        tn.take = local.next_take ? local.next_take : 1;
        take_name = display_name(tn.prefix, take_label(tn));
    }
    r.tape.loaded = present;
    r.tape.label = take_name;
    r.tape.format = tape_format(s.take.format_text);
    r.info.take = present || rec_side || play_side ? upper(take_name) : "NO INPUT";
    r.info.format = present ? s.take.format_text : std::string();
    r.info.time_left = time_left_text(s.take.hours_left);
    if (play_side) {
        r.info.play_pos_ms = std::llround(s.time.position_s * 1000.0);
        r.info.play_len_ms = std::llround(s.time.length_s * 1000.0);
    }
    r.info.limiter = s.settings.limiter;
    r.info.low_cut = s.settings.low_cut;
    r.info.notice = local.notice;
    if (r.info.notice.empty() && !present && !s.card.labels.empty()) r.info.notice = s.card.message;

    r.card.shown = present && s.card.has_level;
    r.card.db = s.card.level_db;
    r.card.can_down = r.card.shown && s.card.level_db > s.card.level_min_db + 0.01;
    r.card.can_up = r.card.shown && s.card.level_db < s.card.level_max_db - 0.01;
    r.soft.shown = true;
    r.soft.db = s.card.soft_gain_db;
    r.soft.can_down = s.card.soft_gain_db > kSoftGainMin + 0.01;
    r.soft.can_up = s.card.soft_gain_db < kSoftGainMax - 0.01;

    /* ---- settings ---- */
    vm.settings.rows = settings_rows(s);

    /* ---- list ---- */
    ui::ListVm &l = vm.list;
    int current = -1, chosen = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        const TakeRow &t = rows[i];
        ui::TakeRow o;
        o.id = t.name;
        o.sticker = t.label.empty() ? sticker_of(t.name) : t.label;
        o.name = display_name(t.name);
        o.detail = take_detail(t, in.now_wall_s, in.utc);
        o.length_ms = std::llround(t.seconds * 1000.0);
        o.prefix = prefix_of(t.name);
        l.takes.push_back(o);
        if (t.current && current < 0) current = static_cast<int>(i);
        if (!local.list_sel.empty() && t.name == local.list_sel) chosen = static_cast<int>(i);
    }
    l.selected = chosen >= 0 ? chosen : current >= 0 ? current : (rows.empty() ? -1 : 0);
    l.take_count = static_cast<int>(rows.size());
    l.folder = s.settings.folder;
    l.free_space = s.take.free_bytes ? size_text(s.take.free_bytes) : std::string();
    l.free_hours = hours_text(s.take.hours_left);
    l.format_short = short_format(s.take.format_text);

    /* ---- trim ---- */
    if (s.trim.open) {
        const TrimSnap &t = s.trim;
        ui::TrimVm &o = vm.trim;
        o.take_id = t.take;
        o.sticker = sticker_of(t.take);
        o.take = upper(display_name(t.take));
        o.length_ms = frames_to_ms(t.frames, t.rate);
        o.marker_ms = frames_to_ms(t.position, t.rate);
        o.span_ms = std::max<int64_t>(1, frames_to_ms(t.visible_frames, t.rate));
        o.in_ms = frames_to_ms(t.in_frame, t.rate);
        o.out_ms = frames_to_ms(t.out_frame, t.rate);
        for (uint64_t sp : t.splits) o.splits.push_back(frames_to_ms(sp, t.rate));
        const int sel = t.selected_split;
        o.selected_split = local.tap_selected && sel >= 0 && sel < static_cast<int>(t.splits.size()) &&
                                   t.splits[static_cast<size_t>(sel)] == t.position
                               ? sel
                               : -1;
        if (in.window) o.window_peaks = peak_levels(*in.window);
        if (in.overview) o.overview_peaks = peak_levels(*in.overview);
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
    }
    return vm;
}

bool same(const ui::RecorderVm &a, const ui::RecorderVm &b)
{
    if (a.input_present != b.input_present || a.transport != b.transport || a.scan_speed != b.scan_speed ||
        a.countdown_left != b.countdown_left || a.countdown_fraction != b.countdown_fraction || a.counter_ms != b.counter_ms)
        return false;
    for (int i = 0; i < static_cast<int>(ui::KeyId::kCount); ++i)
        if (a.keys[i].enabled != b.keys[i].enabled || a.keys[i].latched != b.keys[i].latched) return false;
    if (a.meter.channels != b.meter.channels) return false;
    for (int i = 0; i < 2; ++i)
        if (a.meter.ch[i].bar != b.meter.ch[i].bar || a.meter.ch[i].hold_bar != b.meter.ch[i].hold_bar ||
            a.meter.ch[i].peak_lamp != b.meter.ch[i].peak_lamp)
            return false;
    if (a.tape.loaded != b.tape.loaded || a.tape.label != b.tape.label || a.tape.side != b.tape.side ||
        a.tape.format != b.tape.format)
        return false;
    const ui::InfoVm &x = a.info, &y = b.info;
    if (x.take != y.take || x.format != y.format || x.time_left != y.time_left || x.play_pos_ms != y.play_pos_ms ||
        x.play_len_ms != y.play_len_ms || x.limiter != y.limiter || x.low_cut != y.low_cut || x.notice != y.notice)
        return false;
    auto gain = [](const ui::GainVm &g, const ui::GainVm &h) {
        return g.shown == h.shown && g.db == h.db && g.can_down == h.can_down && g.can_up == h.can_up;
    };
    return gain(a.card, b.card) && gain(a.soft, b.soft);
}

bool same(const ui::ViewModel &a, const ui::ViewModel &b)
{
    return same(a.rec, b.rec) && a.settings == b.settings && a.list == b.list && a.trim == b.trim && a.save == b.save &&
           a.confirm == b.confirm;
}

} // namespace bind
} // namespace cassette
