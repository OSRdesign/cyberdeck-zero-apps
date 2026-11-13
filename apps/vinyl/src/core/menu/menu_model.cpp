/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl Menu (see menu_model.hpp).
 */
#include "menu_model.hpp"

#include <cstdio>

namespace vinyl {
namespace menu {

const char *page_title(Page p) {
    switch (p) {
    case Page::Sleep: return "Sleep timer";
    case Page::Playback: return "Playback";
    case Page::ReplayGain: return "ReplayGain";
    case Page::Keys: return "Keys";
    case Page::About: return "About";
    default: return "Menu";
    }
}

namespace {

Row heading(const char *t) {
    Row r;
    r.kind = Row::Kind::Heading;
    r.text = t;
    return r;
}

Row open(const char *id, const char *text, const std::string &value = std::string()) {
    Row r;
    r.kind = Row::Kind::Open;
    r.id = id;
    r.text = text;
    r.value = value;
    return r;
}

Row edit(const char *id, const char *text, const std::string &value) {
    Row r = open(id, text, value);
    r.kind = Row::Kind::Edit;
    return r;
}

Row choice(const std::string &id, const char *text, bool chosen) {
    Row r;
    r.kind = Row::Kind::Choice;
    r.id = id;
    r.text = text;
    r.chosen = chosen;
    return r;
}

const char *on_off(bool v) { return v ? "On" : "Off"; }

}  // namespace

std::vector<Row> menu_rows(const State &s) {
    /* only what is built (owner decision D32): the canvas's Equalizer and Library rows come with their phases; Keys and
     * About are rows of their own (the brief of P2a) */
    return {heading("TOOLS"),
            edit("sleep", "Sleep timer", sleep_menu_value(s.sleep, s.sleep_left_ms)),
            heading("SETTINGS"),
            open("playback", "Playback"),
            open("output", "Output", s.output),
            open("keys", "Keys"),
            open("about", "About")};
}

std::string sleep_id(SleepMode m) {
    switch (m) {
    case SleepMode::Min15: return "15";
    case SleepMode::Min30: return "30";
    case SleepMode::Min60: return "60";
    case SleepMode::Track: return "track";
    case SleepMode::Album: return "album";
    default: return "off";
    }
}

bool sleep_from_id(const std::string &id, SleepMode &out) {
    for (SleepMode m : kSleepModes)
        if (sleep_id(m) == id) {
            out = m;
            return true;
        }
    return false;
}

std::vector<Row> sleep_rows(SleepMode chosen) {
    std::vector<Row> rows;
    for (SleepMode m : kSleepModes) rows.push_back(choice(sleep_id(m), sleep_mode_word(m), m == chosen));
    return rows;
}

std::vector<Row> playback_rows(const Settings &s) {
    /* no Speed row (D32: "Speed (later)" dropped); no Crossfade row (P2d) */
    return {edit("gapless", "Gapless", on_off(s.gapless)), edit("replaygain", "ReplayGain", replaygain_mode_word(s.replaygain)),
            edit("remember", "Remember position in long files", on_off(s.remember_long))};
}

std::vector<Row> replaygain_rows(ReplayGainMode chosen) {
    std::vector<Row> rows;
    for (ReplayGainMode m : {ReplayGainMode::Off, ReplayGainMode::Track, ReplayGainMode::Album})
        rows.push_back(choice(replaygain_mode_name(m), replaygain_mode_word(m), m == chosen));
    return rows;
}

std::vector<Row> rows_of(Page p, const State &s) {
    switch (p) {
    case Page::Menu: return menu_rows(s);
    case Page::Sleep: return sleep_rows(s.sleep);
    case Page::Playback: return playback_rows(s.settings);
    case Page::ReplayGain: return replaygain_rows(s.settings.replaygain);
    default: return {};
    }
}

std::string page_heading(Page p) {
    if (p == Page::Sleep) return "STOP PLAYING AFTER";
    if (p == Page::ReplayGain) return "REPLAYGAIN";
    return "";
}

std::string page_hint(Page p, const State &s) {
    switch (p) {
    case Page::Sleep: return sleep_status_text(s.sleep, s.sleep_left_ms);
    case Page::Playback: return "ReplayGain evens out the loudness between tracks.";
    case Page::ReplayGain:
        return "Track: every track as loud as the others. Album: an album keeps its own loudness steps. Files without "
               "ReplayGain tags play as they are.";
    default: return "";
    }
}

int first_item(const std::vector<Row> &rows) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind != Row::Kind::Heading) return static_cast<int>(i);
    return -1;
}

int step_item(const std::vector<Row> &rows, int i, int d) {
    const int n = static_cast<int>(rows.size());
    if (i < 0 || i >= n) return first_item(rows);
    int best = i;
    for (int k = i + (d > 0 ? 1 : -1), left = d > 0 ? d : -d; k >= 0 && k < n && left > 0; k += d > 0 ? 1 : -1)
        if (rows[static_cast<size_t>(k)].kind != Row::Kind::Heading) {
            best = k;
            --left;
        }
    return best;
}

int initial_item(const std::vector<Row> &rows) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Kind::Choice && rows[i].chosen) return static_cast<int>(i);
    return first_item(rows);
}

/* ------------------------------------------------------------------------------------------ the key map */

KeyCmd letter_command(uint32_t evdev, bool queue_page) {
    switch (evdev) {
    case kKeySpace: return KeyCmd::Toggle;
    case kKeyS: return KeyCmd::Shuffle;
    case kKeyR: return KeyCmd::Repeat;
    case kKeyQ: return KeyCmd::Queue;
    case kKeyB: return KeyCmd::Browse;
    case kKeyO: return KeyCmd::Output;
    case kKeyM: return KeyCmd::Menu;
    case kKeyT: return KeyCmd::Sleep;
    case kKeyMinus: return KeyCmd::VolumeDown;
    case kKeyEqual: return KeyCmd::VolumeUp;
    case kKeyComma: return KeyCmd::Prev;
    case kKeyDot: return KeyCmd::Next;
    case kKeyU: return queue_page ? KeyCmd::QueueUp : KeyCmd::None;
    case kKeyD: return queue_page ? KeyCmd::QueueDown : KeyCmd::None;
    case kKeyX: return queue_page ? KeyCmd::QueueRemove : KeyCmd::None;
    case kKeyC: return queue_page ? KeyCmd::QueueClear : KeyCmd::None;
    default: return KeyCmd::None;
    }
}

std::vector<KeyHelp> key_help() {
    /* the canvas's rows (Keys-480x320, Keys-320x480) first, then the rest of the key map; the page scrolls */
    return {
        {"Space", "Play or pause", {kKeySpace}, {KeyCmd::Toggle}, false},
        {"Left", "Restart, or previous; hold: rewind", {}, {}, false},
        {"Right", "Next; hold: fast forward", {}, {}, false},
        {"Shift+Left, Right", "Seek 10 seconds", {}, {}, false},
        {"S", "Shuffle", {kKeyS}, {KeyCmd::Shuffle}, false},
        {"R", "Repeat", {kKeyR}, {KeyCmd::Repeat}, false},
        {"Q, B, O, M", "Queue, Browse, Output, Menu", {kKeyQ, kKeyB, kKeyO, kKeyM},
         {KeyCmd::Queue, KeyCmd::Browse, KeyCmd::Output, KeyCmd::Menu}, false},
        {"- and =", "Volume down and up", {kKeyMinus, kKeyEqual}, {KeyCmd::VolumeDown, KeyCmd::VolumeUp}, false},
        {"T", "Sleep timer", {kKeyT}, {KeyCmd::Sleep}, false},
        {", and .", "Previous and next, on every page", {kKeyComma, kKeyDot}, {KeyCmd::Prev, KeyCmd::Next}, false},
        {"Up, Down, Enter", "Lists: move, choose", {}, {}, false},
        {"Left, Right", "Lists: back, choose", {}, {}, false},
        {"U, D", "Queue: move up, move down", {kKeyU, kKeyD}, {KeyCmd::QueueUp, KeyCmd::QueueDown}, true},
        {"X or Delete", "Queue: remove", {kKeyX}, {KeyCmd::QueueRemove}, true},
        {"C, then Y", "Queue: clear all", {kKeyC}, {KeyCmd::QueueClear}, true},
        {"Esc", "Back; at Browse: quit", {}, {}, false},
        {"Media keys", "Play, pause, next, previous, stop", {}, {}, false},
    };
}

/* ------------------------------------------------------------------------------------------ About */

std::string about_output_line(const std::string &label, const OutputVolumeModel &m, bool known, bool software) {
    const std::string name = label.empty() ? std::string("The output") : label;
    if (!known) return name + ": the level is not known yet.";
    char b[160];
    if (!m.db) std::snprintf(b, sizeof b, ": %d steps (no dB scale).", m.n);
    else
        std::snprintf(b, sizeof b, ": %.1f to %.1f dB, %d levels of %.2f dB%s.", m.lo_db, m.hi_db, m.n, m.step_db(),
                      software ? " (Vinyl's own volume: the card has no volume control)" : "");
    return name + b;
}

About about(const std::string &version, const std::string &output_line) {
    About a;
    a.name = "Vinyl " + version;
    a.line = "A music player for CyberdeckZero Launcher boards. MIT licence.";
    a.formats = "MP3, FLAC, Ogg Vorbis, Opus, WAV and AIFF with small decoders; AAC, ALAC, WMA and the rest through FFmpeg.";
    a.output = output_line;
    a.libraries = "libmpg123, libvorbisfile, libFLAC, libopus, libasound, libpulse, FFmpeg libraries, LVGL, stb_image.";
    a.licences = "Full credits and licences: /usr/share/doc/vinyl/copyright";
    return a;
}

}  // namespace menu
}  // namespace vinyl
