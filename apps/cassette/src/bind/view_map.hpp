/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the controller's state (Snapshot, takes(), the peaks) to the UI's ViewModel (ui/view_model.hpp).
 * Pure: plain values in, plain values out, no LVGL, no threads (tests/bind/test_map.cpp checks every field).
 *
 * The rules (the controller's facts mapped onto what the canvas boards show):
 *   recorder  transport = the deck state; keys enabled as the deck says, a latched key never dimmed; REC latched while
 *             recording (not during the countdown, as the board CassetteCount), PLAY/PAUSE as ui_format.hpp play_key_look
 *             (latched while a take plays, scans or is paused and while a recording is paused; not while recording),
 *             REW / FWD latched while held or scanning; REC dimmed without a sound input. The hubs: hub_speed_dps of the
 *             transport, or of a 5 s jump while it flashes (ScanSnap::jump_dir). The meter: the INPUT level (idle,
 *             countdown, recording); the PLAYED level (Snapshot::play_meter) on L and R while a take plays, scans or is
 *             paused, a mono take on both bars, the PEAK lamp off.
 *             The counter: the take's length while recording, the position while playing, else 0. The cassette and the
 *             info strip name the take being recorded, the take playing, else the take REC would write next ("Take 008").
 *             No sound input (the capture path is not running): the empty window, "NO INPUT", no CARD stepper; the card's
 *             message goes to the strip when a card is listed but cannot be used (busy, unsupported).
 *   names     "Take 006-1": the file's prefix with its first letter raised, then its label ("006-1", take_label).
 *   settings  the board's rows; Input level and Auto gain only when the card has them; Channels shown and dimmed (the
 *             controller has no channel setting); Format lists every format of the model (codec/formats.hpp) with its size
 *             per minute at the card's capture format and its free hours, a format this card or the libraries cannot
 *             record dimmed with the reason (format_picker()); its value is the effective choice ("FLAC", "Opus 32 kbps",
 *             "WAV 48 kHz 16-bit", "WAV (FLAC unavailable)" when the stored choice cannot be used); Format, Folder and
 *             Input dimmed while a take or the countdown runs.
 *   list      newest first (the controller's order); the highlight = the take the user moved it to, else the current take.
 *   trim      times in ms from frames at the take's rate; span = the frames the zoom shows; the window's peaks are centred on
 *             the marker; a flag shows selected only after a tap on it and while the marker stays on it.
 */
#pragma once

#include "core/app/controller.hpp"
#include "ui/view_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {
namespace bind {

/* What the binding keeps itself on the UI thread (the controller has no such state). */
struct LocalState {
    bool save_shown = false;
    std::vector<ui::SaveFile> save_files;
    enum class Confirm { None, Replace, Delete };
    Confirm confirm = Confirm::None;
    std::string confirm_take;    /* Delete: the take's file name */
    std::string list_sel;        /* the highlighted take (file name); empty or gone: the current take */
    bool tap_selected = false;   /* the trim page's selected flag came from a tap */
    std::string notice;          /* the notice shown now ("" none) */
    uint64_t next_take = 0;      /* the number REC hands out next; 0 = not known yet (1 is shown) */
};

struct MapInput {
    const Snapshot *snap = nullptr;
    const std::vector<TakeRow> *rows = nullptr;
    const std::vector<PeakBar> *window = nullptr;   /* the trim window's peaks (centred on the marker) */
    const std::vector<PeakBar> *overview = nullptr; /* the whole take */
    const LocalState *local = nullptr;
    int64_t now_wall_s = 0;  /* "today" / "yesterday" in the list */
    bool utc = false;        /* tests: dates in UTC instead of local time */
};

ui::ViewModel map_view(const MapInput &in);

/* ---- the pieces (tested one by one) ---- */
ui::Transport transport_of(int deck_state);
bool recording_family(int deck_state); /* countdown, recording, recording paused */
bool playing_family(int deck_state);   /* playing, paused, scanning */
/* "Take 006-1" from a file name; a name outside the grammar gives its label as is. */
std::string display_name(const std::string &file_name);
std::string display_name(const std::string &prefix, const std::string &label);
std::string upper(const std::string &s);
std::string sticker_of(const std::string &file_name); /* "006-1"; the stem outside the grammar */
std::string prefix_of(const std::string &file_name);  /* the editable part; "" outside the grammar */
/* "12 h 40", "0 h 05", "117 h" from 100 h; "" for 0 or less */
std::string time_left_text(double hours);
/* "117 h", "40 min" (the list strip); "" for 0 or less */
std::string hours_text(double hours);
/* "512 KB", "4 MB", "4.2 GB", "41 GB" (1024-based) */
std::string size_text(uint64_t bytes);
/* "42 s", "3 min", "1 h 02" */
std::string length_text(double seconds);
/* "today 14:32", "yesterday 18:20", "2 Nov", "2 Nov 2025", "nodate" from "YYYY-MM-DD HH:MM" */
std::string date_part(const std::string &date_text, int64_t now_wall_s, bool utc);
/* "today 14:32 · 42 s · 4 MB · FLAC" (the row's format tag when it has one); a damaged take "damaged · 4 MB"; a
 * repaired one ends in " · repaired" */
std::string take_detail(const TakeRow &r, int64_t now_wall_s, bool utc);
/* "48k/16 WAV" -> "WAV 48k" (the cassette label), "48k/16" (the list strip), "WAV 48 kHz 16-bit" (the Format row) */
std::string tape_format(const std::string &format_text);
std::string short_format(const std::string &format_text);
std::string format_words(const std::string &format_text);
/* The Format picker. size_per_minute_text: "5.8 MB/min" (1 MB = 10^6 bytes; one decimal under 10 MB, none from 10), under
 * 1 MB "190 kB/min" rounded to 5 kB; "about " in front of an estimate (SizeKind::About, FLAC); "" without a figure. */
std::string size_per_minute_text(const SizeEstimate &size);
/* "Sizes at 48 kHz mono 16-bit, this card"; "No sound input: sizes unknown" without a capture format */
std::string formats_footer_text(const FormatInputs &in);
struct FormatPicker {
    std::vector<FormatChoice> list;        /* the model's list for this card, in the Format row's order */
    std::vector<ui::FormatEntry> entries;  /* the same, as the page shows it */
    int current = -1;                      /* the entry the recorder uses (effective_choice), -1: none */
    std::string value;                     /* the Settings row's value text */
    std::string footer;
};
FormatPicker format_picker(const Snapshot &s);
/* The selection rule: the entry `index` of the list when it can be chosen; null when the index is out of range (notice
 * stays empty) or the entry is not compatible (*notice = "Not available: <reason>"). */
const FormatChoice *selectable_format(const std::vector<FormatChoice> &list, int index, std::string *notice);
/* 0..1 from a 16-bit min/max pair (the louder side) */
float peak_level(const PeakBar &b);
std::vector<float> peak_levels(const std::vector<PeakBar> &bars);
std::vector<ui::SettingRow> settings_rows(const Snapshot &s, uint64_t next_take = 0);
/* The record timer's choices (seconds) in row order; and the deepest zoom level whose window is still >= 2 s (or the
 * whole take when it is shorter): the plan's "from the whole take down to a 2 s window". */
extern const int kTimerChoices[4];
/* The "Sync every" choices (seconds, the controller's 5..30 range) in row order. */
extern const int kSyncChoices[4];
int zoom_limit(const TrimSnap &t, double min_window_s = 2.0);
int64_t frames_to_ms(uint64_t frames, uint32_t rate);
int64_t ms_to_frames(int64_t ms, uint32_t rate);

/* Equality of everything the UI draws (the glue skips CassetteUi::update when nothing changed). */
bool same(const ui::RecorderVm &a, const ui::RecorderVm &b);
bool same(const ui::ViewModel &a, const ui::ViewModel &b);

} // namespace bind
} // namespace cassette
