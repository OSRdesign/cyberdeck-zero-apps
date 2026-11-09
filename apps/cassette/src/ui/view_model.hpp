/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the view model of the user interface (task T6b) and the callbacks the screens issue.
 *
 * The UI (ui/cassette_ui.hpp) draws ONLY what this model says and never touches the core: the application controller
 * (src/core/app, task T6a) fills a ViewModel from its state (Controller::snapshot() mirrors these structs) and calls
 * CassetteUi::update(vm) whenever something changed (the meter at about 30 Hz while it runs). Every command a screen
 * issues goes out through UiActions; the UI changes nothing by itself except which page is shown, the selection
 * highlight of a list, and the scroll positions (it tells the controller about page changes through UiActions::open).
 *
 * Plain values only (no LVGL, no core types), so the model can be built and compared in unit tests. Times are
 * milliseconds; texts are UTF-8 and already in the user's words (the UI adds only its fixed labels and joins the
 * parts it is given, see ui_format.hpp). An empty std::function in UiActions is allowed: that command does nothing.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cassette {
namespace ui {

/* ------------------------------------------------------------------------------------------------ the recorder page */

/* The five piano keys, left to right. */
enum class KeyId { Rew = 0, PlayPause, Fwd, Stop, Rec, kCount };

/* What the transport does now: decides the PLAY/PAUSE key's glyph (ui_format.hpp play_key_look) and the info strip's
 * wording; the hubs turn at RecorderVm::hub_dps (filled from ui_format.hpp hub_speed_dps). Mirrors core DeckState. */
enum class Transport { Idle, Countdown, Recording, RecordingPaused, Playing, PlayingPaused, ScanForward, ScanReverse };

struct KeyState {
    bool enabled = true;  /* false: dimmed (35 %), presses are still reported (the controller ignores them) */
    bool latched = false; /* drawn pressed down (dark red), e.g. REC while recording, REW while scanning */
};

/* One metered channel: the LED bar has 15 steps over -40..0 dBFS (core Meter: step i lit when peak >= -40 + i*40/14). */
struct ChannelMeter {
    int bar = 0;            /* lit steps 0..15 (core ChannelLevel::bar) */
    int hold_bar = 0;       /* peak-hold tick after this many steps, 0 = no tick (core ChannelLevel::hold_bar) */
    bool peak_lamp = false; /* PEAK lamp: a sample at -1 dBFS or above within the last second */
};

struct MeterVm {
    int channels = 1;   /* 1: one bar "IN"; 2: two bars "L" and "R" */
    ChannelMeter ch[2];
};

/* The cassette in the window. */
struct TapeVm {
    bool loaded = true;          /* false: the empty window (no sound input) */
    std::string label;           /* the handwritten name, "Take 007" */
    std::string side = "SIDE A";
    std::string format;          /* "WAV 48k" */
};

/* A gain stepper (CARD = the sound card's capture level, SOFT = software gain on top). */
struct GainVm {
    bool shown = true;           /* CARD: only when the card has a capture level control; SOFT: always */
    double db = 0.0;             /* shown "+13.0 dB" (one decimal) where it fits, "+13 dB" in the narrow layout */
    bool can_down = true, can_up = true; /* false: that button dimmed */
};

/* What the info strip and the portrait info box say (joined by ui_format.hpp info_line / info_title / info_detail). */
struct InfoVm {
    std::string take;            /* "TAKE 007" (recording, idle) or the playing take "TAKE 006" */
    std::string format;          /* "48k/16 WAV" */
    std::string time_left;       /* "12 h 40" (the strip adds " left") */
    std::int64_t play_pos_ms = 0, play_len_ms = 0; /* while a take plays or is paused */
    bool limiter = false;        /* LIM lamp */
    bool low_cut = false;        /* LOW CUT lamp */
    bool server = false;         /* the capture path is the sound server (the card is held by another program) */
    std::string card;            /* idle and through the server: the card's label, shown first on the strip ("" otherwise) */
    std::int64_t lost_ms = 0;    /* recording: audio lost to capture overruns so far (0 = none); shown in red */
    std::string notice;          /* a standing message that replaces the strip's text (the card's "no sound input" reason); the
                                  * timed notices of the controller and the binding are ViewModel::notice */
};

struct RecorderVm {
    bool input_present = true;   /* false: the "no sound input" state (empty window, message, REC dimmed) */
    Transport transport = Transport::Idle;
    KeyState keys[static_cast<int>(KeyId::kCount)];
    int scan_speed = 5;          /* the badge "<< 5x" / ">> 5x" while scanning */
    /* The hubs' angular speed in degrees per second, counterclockwise positive, left and right (ui_format.hpp
     * hub_speed_dps: both the same, a real cassette's hubs turn together): play / record +150, FWD or a forward jump
     * +450, REW or a backward jump -450, otherwise 0 (still). */
    double hub_dps[2] = {0.0, 0.0};
    int countdown_left = 0;      /* Countdown: the big number (seconds left) */
    float countdown_fraction = 1.0f; /* Countdown: the red ring's remaining part, 1 = full, 0 = empty */
    std::int64_t counter_ms = 0; /* the counter: MM:SS, H:MM:SS from one hour (smaller digits) */
    MeterVm meter;
    TapeVm tape;
    InfoVm info;
    GainVm card;                 /* CARD stepper */
    GainVm soft;                 /* SOFT stepper */
};

/* ------------------------------------------------------------------------------------------------ settings */

enum class SettingKind {
    Choice, /* a value with a chevron; a tap opens the picker of `choices` (UiActions::setting_choice) */
    Toggle, /* a switch (UiActions::setting_toggle) */
    Slider, /* a slider with `value_text` beside it, min..max (UiActions::setting_slider) */
    Text,   /* a value with a chevron; a tap opens a one-line text field (UiActions::setting_text) */
    Info,   /* a read-only value without a chevron (dimmed with enabled = false), e.g. Channels */
};

struct SettingRow {
    std::string id;              /* the controller's key, given back in every callback ("input", "format", ...) */
    std::string label;           /* "Input level" */
    SettingKind kind = SettingKind::Choice;
    bool enabled = true;         /* false: dimmed and not changeable (format, folder while a take records) */
    std::string value;           /* Choice / Text: the shown value ("WAV 48 kHz 16-bit") */
    std::vector<std::string> choices; /* Choice: the picker's entries */
    int choice = 0;              /* Choice: the current entry */
    bool on = false;             /* Toggle */
    int min = 0, max = 16, pos = 0; /* Slider */
    std::string value_text;      /* Slider: "8 / 16" */
};

/* The rows in order. The controller leaves out what the card does not have (Input level, Auto gain). */
struct SettingsVm {
    std::vector<SettingRow> rows;
};

/* ------------------------------------------------------------------------------------------------ recordings list */

struct TakeRow {
    std::string id;              /* the controller's key (a file name) */
    std::string sticker;         /* the label sticker: "007", "006-1" */
    std::string name;            /* "Take 007" */
    std::string detail;          /* "today 14:32 · 42 s · 4 MB", "nodate · 41 s · 4 MB" */
    std::int64_t length_ms = 0;  /* the duration column (landscape) */
    std::string prefix;          /* the editable part of the name (rename) */
};

struct ListVm {
    std::vector<TakeRow> takes;  /* newest first */
    int selected = 0;            /* the darker row; the four keys act on it (-1: none) */
    int take_count = 0;          /* the strip: "12 TAKES" */
    std::string folder;          /* "~/Recordings" */
    std::string free_space;      /* "41 GB" */
    std::string free_hours;      /* "117 h" */
    std::string format_short;    /* "48k/16" */
};

/* ------------------------------------------------------------------------------------------------ trim and split */

struct TrimVm {
    std::string take_id;
    std::string sticker;         /* "006" */
    std::string take;            /* "TAKE 006" */
    std::int64_t length_ms = 0;  /* the whole take */
    std::int64_t marker_ms = 0;  /* the red play marker, always in the middle of the waveform */
    std::int64_t span_ms = 60000; /* ZOOM: the window shown, from the whole take down to 2 s */
    std::int64_t in_ms = 0, out_ms = 0;
    std::vector<std::int64_t> splits; /* split points, ascending; flags are numbered 1.. in this order */
    int selected_split = -1;     /* the flag with the white outline (-1: none); the SPLIT key then reads REMOVE */
    /* Peaks 0..1 (any count; the UI resamples to its bar count with the maximum of each bucket):
     * window_peaks cover marker - span/2 .. marker + span/2 (zero where that is outside the take),
     * overview_peaks cover the whole take. */
    std::vector<float> window_peaks;
    std::vector<float> overview_peaks;
    bool playing = false;        /* PLAY latched */
};

/* The save box (opened by SAVE on the trim page; UiActions::trim_save asks for it). */
struct SaveFile {
    std::string name;            /* "take_006_20261108-1105-1.wav" (the narrow layout drops ".wav") */
    std::int64_t length_ms = 0;
};

struct SaveVm {
    bool shown = false;
    std::vector<SaveFile> files;
};

/* A small yes / no box ("Delete the original take?"). */
struct ConfirmVm {
    bool shown = false;
    std::string title;           /* "Delete the original take?" */
    std::string body;            /* one or two lines under it (may be empty) */
    std::string yes = "DELETE";  /* the red key; the other one is CANCEL */
};

/* ------------------------------------------------------------------------------------------------ everything */

struct ViewModel {
    /* A notice (a message of the controller or the binding, the binding times it: 4 s, one after the other). Shown on
     * every page in the place of that page's own info line (see cassette_ui.hpp); empty: none. */
    std::string notice;
    RecorderVm rec;
    SettingsVm settings;
    ListVm list;
    TrimVm trim;
    SaveVm save;
    ConfirmVm confirm;
};

/* Equality of the parts the pages compare to skip work when nothing changed (the meter updates 30 times a second). */
inline bool operator==(const SettingRow &a, const SettingRow &b)
{
    return a.id == b.id && a.label == b.label && a.kind == b.kind && a.enabled == b.enabled && a.value == b.value &&
           a.choices == b.choices && a.choice == b.choice && a.on == b.on && a.min == b.min && a.max == b.max &&
           a.pos == b.pos && a.value_text == b.value_text;
}
inline bool operator==(const SettingsVm &a, const SettingsVm &b) { return a.rows == b.rows; }
inline bool operator==(const TakeRow &a, const TakeRow &b)
{
    return a.id == b.id && a.sticker == b.sticker && a.name == b.name && a.detail == b.detail && a.length_ms == b.length_ms &&
           a.prefix == b.prefix;
}
inline bool operator==(const ListVm &a, const ListVm &b)
{
    return a.takes == b.takes && a.selected == b.selected && a.take_count == b.take_count && a.folder == b.folder &&
           a.free_space == b.free_space && a.free_hours == b.free_hours && a.format_short == b.format_short;
}
inline bool operator==(const TrimVm &a, const TrimVm &b)
{
    return a.take_id == b.take_id && a.sticker == b.sticker && a.take == b.take && a.length_ms == b.length_ms &&
           a.marker_ms == b.marker_ms && a.span_ms == b.span_ms && a.in_ms == b.in_ms && a.out_ms == b.out_ms &&
           a.splits == b.splits && a.selected_split == b.selected_split && a.window_peaks == b.window_peaks &&
           a.overview_peaks == b.overview_peaks && a.playing == b.playing;
}
inline bool operator==(const SaveFile &a, const SaveFile &b) { return a.name == b.name && a.length_ms == b.length_ms; }
inline bool operator==(const SaveVm &a, const SaveVm &b) { return a.shown == b.shown && a.files == b.files; }
inline bool operator==(const ConfirmVm &a, const ConfirmVm &b)
{
    return a.shown == b.shown && a.title == b.title && a.body == b.body && a.yes == b.yes;
}

/* The pages. The UI owns which one shows (L, S, Esc, the header buttons, TRIM) and reports every change. */
enum class Screen { Recorder, Settings, List, Trim };

/* A piano key's touch or keyboard key (docs/KEYS.md): Down and Up for every key (REW / FWD scan while held), then
 * Click when the finger is lifted on the key or the keyboard key is released. The controller acts on Click for
 * PLAY/PAUSE, STOP and REC, on Down / Up for REW and FWD. */
enum class KeyEdge { Down, Up, Click };

enum class SaveMode { NewFiles, Replace, Cancel };

struct UiActions {
    /* recorder */
    std::function<void(KeyId, KeyEdge)> deck_key;
    std::function<void()> countdown_cancel;           /* a tap on the countdown panel, or Esc during the countdown */
    std::function<void(int dir)> card_gain;           /* CARD -1 / +1 step */
    std::function<void(int dir)> soft_gain;           /* SOFT -1 / +1 step */
    /* navigation: the page now shown (Recorder after Esc, List, Settings, Trim after TRIM) */
    std::function<void(Screen)> open;
    /* a tap on the notice that covers the counter row of the narrow recorder: dismisses it (the next one may follow) */
    std::function<void()> notice_dismiss;
    /* settings (id = SettingRow::id) */
    std::function<void(const std::string &id, int choice)> setting_choice;
    std::function<void(const std::string &id, bool on)> setting_toggle;
    std::function<void(const std::string &id, int pos)> setting_slider;
    std::function<void(const std::string &id, const std::string &text)> setting_text;
    /* recordings list (index into ListVm::takes) */
    std::function<void(int index)> list_select;       /* a tap or Up / Down moved the highlight */
    std::function<void(int index)> list_activate;     /* Enter, or a tap on the selected row */
    std::function<void(int index)> list_play;
    std::function<void(int index, const std::string &prefix)> list_rename; /* the edited prefix, after RENAME */
    std::function<void(int index)> list_trim;         /* TRIM: the UI shows the trim page; load TrimVm for that take */
    std::function<void(int index)> list_delete;       /* DELETE: confirm first (ConfirmVm), delete on confirm_answer(true) */
    /* trim and split */
    std::function<void(int dir)> trim_zoom;           /* ZOOM -1 (wider window) / +1 (narrower) */
    std::function<void(std::int64_t delta_ms)> trim_nudge; /* POSITION buttons, Left / Right: 100 ms, Shift 1000 ms */
    std::function<void(std::int64_t marker_ms)> trim_scrub; /* a sideways drag of the waveform: the new marker */
    std::function<void()> trim_set_in;
    std::function<void()> trim_set_out;
    std::function<void()> trim_split;                 /* add a split point at the marker */
    std::function<void(int split)> trim_remove_split; /* SPLIT reads REMOVE while a flag is selected */
    std::function<void(int split)> trim_select_split; /* a tap on a flag: select it and move the marker to it (-1: none) */
    std::function<void()> trim_play;
    std::function<void()> trim_save;                  /* SAVE: show the save box (SaveVm::shown) */
    /* boxes */
    std::function<void(SaveMode)> save_choose;
    std::function<void(bool yes)> confirm_answer;
};

} // namespace ui
} // namespace cassette
