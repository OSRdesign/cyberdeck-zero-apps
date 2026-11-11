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
 *             controller has no channel setting); the recording format is three rows, Codec, Sample rate and Quality (Bit
 *             depth for WAV and FLAC, Bit rate for Opus), each with a picker of its own (codec_picker(), rate_picker(),
 *             quality_picker(): the model of codec/formats.hpp with the size per minute of every entry at the other two
 *             choices and its free hours, an entry this card or the libraries cannot record dimmed with the reason, except
 *             a bit depth the card cannot record, which is left out; WAV and FLAC on a card with one depth have no Quality
 *             row at all, the depth being the card's); the
 *             values are the effective choice ("FLAC", "48 kHz", "16-bit" / "32 kbps"; "WAV (Opus unavailable)" when
 *             the stored codec cannot be used); Codec, Sample rate, Quality, Folder and Input dimmed while a take or the
 *             countdown runs.
 *   list      newest first (the controller's order); the highlight = the take the user moved it to, else the current take.
 *             MERGE is dimmed with fewer than two takes. Merge mode (LocalState::merge_mode): each row's order number in the
 *             choice (0: not chosen), whether it can be chosen and why not (merge_row_reason), the merge strip; the merge box
 *             (the plan the controller gave), the confirm before deleting the originals, the progress box while a merge runs.
 *   trim      times in ms from frames at the take's rate; span = the frames the zoom shows; the window's peaks are centred on
 *             the marker; a flag shows selected only after a tap on it and while the marker stays on it.
 */
#pragma once

#include "core/app/controller.hpp"
#include "core/codec/formats.hpp"
#include "ui/ui_format.hpp"
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
    enum class Confirm { None, Replace, Delete, MergeDelete };
    Confirm confirm = Confirm::None;
    /* MERGE TAKES: merge mode on the list page, the merge box, the merge the binding started */
    bool merge_mode = false;
    std::vector<std::string> merge_sel; /* the chosen takes (file names), in the order chosen */
    bool merge_box = false;             /* the merge box shows merge_info */
    MergeInfo merge_info;               /* the controller's plan of the chosen takes */
    bool merge_running = false;         /* a merge was asked for and its end has not been seen */
    int merge_shown_pct = 0;            /* the progress box's percent */
    int merge_count = 0;                /* the takes of the running merge */
    std::string merge_name;             /* its new take's name (the plan's until the controller says) */
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
    /* window on the take's bar grid (ui::BarGrid): window[i] is grid bar window_first + i of window_fpb frames;
     * window_fpb 0: spread evenly over the window */
    int64_t window_first = 0, window_fpb = 0;
    /* the trim marker shown (frames), when the binding follows a playing preview smoothly (Binder); -1: the snapshot's */
    int64_t trim_position = -1;
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
std::string date_part(const std::string &date_text, int64_t now_wall_s, bool utc, bool shortest = false);
/* "today 14:32 · 42 s · 4 MB · FLAC" (the row's format tag when it has one); a damaged take "damaged · 4 MB"; a
 * repaired one ends in " · repaired" */
std::string take_detail(const TakeRow &r, int64_t now_wall_s, bool utc);
/* the detail line in its forms, longest first (the full one is take_detail): then without the size, then also with the
 * date in its shortest form. The format tag is in every form. A narrow list shows the longest one that fits one line. */
std::vector<std::string> take_detail_forms(const TakeRow &r, int64_t now_wall_s, bool utc);
/* "48k/16 WAV" -> "WAV 48k" (the cassette label), "48k/16" (the list strip), "WAV 48 kHz 16-bit" (the Format row) */
std::string tape_format(const std::string &format_text);
std::string short_format(const std::string &format_text);
std::string format_words(const std::string &format_text);
/* The pickers' size. size_per_minute_text: "5.8 MB/min" (1 MB = 10^6 bytes; one decimal under 10 MB, none from 10), under
 * 1 MB "190 kB/min" rounded to 5 kB; "about " in front of an estimate (SizeKind::About, FLAC); "" without a figure. */
std::string size_per_minute_text(const SizeEstimate &size);
/* "Sizes at 48 kHz mono 16-bit, this card"; "No sound input: sizes unknown" without a capture format (the three pickers
 * word their own footers: the other two choices) */
std::string formats_footer_text(const FormatInputs &in);
/* The three pickers (Settings: Codec, Sample rate, Quality). Entries in the page's order: the Codec picker has two
 * group headings ("Codecs", then "Quick presets"), which items skip (header). Choosing an entry: a preset sets the codec and its
 * bit rate (rate and quality back to their defaults); any other entry carries the stored values it leads to (change), with
 * the notice naming what else moved (a rate the new codec cannot record). */
struct ParamItem {
    bool header = false;
    bool preset = false;
    FormatKey key;          /* preset */
    StoredChange change;    /* not a preset */
    bool compatible = true; /* false: dimmed, a tap says why */
    std::string why;
};
struct ParamPicker {
    std::string label;                    /* the row's label ("Bit depth" or "Bit rate" for the third) */
    std::vector<ParamItem> items;
    std::vector<ui::FormatEntry> entries; /* the same, as the page shows it */
    int current = -1;                     /* the entry the recorder uses, -1: none */
    std::string value;                    /* the Settings row's value text ("FLAC", "48 kHz", "16-bit", "32 kbps") */
    std::string footer;                   /* what the sizes assume: the other two choices */
};
ParamPicker codec_picker(const Snapshot &s);
ParamPicker rate_picker(const Snapshot &s);
ParamPicker quality_picker(const Snapshot &s);
/* The selection rule: the entry `index` when it can be chosen; null when the index is out of range or a heading (notice
 * stays empty) or the entry is not compatible (*notice = "Not available: <reason>"). */
const ParamItem *selectable_param(const ParamPicker &p, int index, std::string *notice);
/* MERGE TAKES. Why row `i` cannot be chosen now ("" when it can): a take in use first ("Playing: stop it first", "Open on
 * the Edit page", "Being recorded"), then core/edit/merge.hpp's rule against the FIRST take chosen ("Different format: FLAC
 * 44.1 kHz", "Different rate", "Different channels", "Different bit depth"), or, with nothing chosen yet (or for the first
 * take itself), the take's own ("Not a Cassette take", "Damaged take"). */
std::string merge_row_reason(const Snapshot &s, const std::vector<TakeRow> &rows, const std::vector<std::string> &chosen,
                             size_t i);
/* The merge strip: "Merge · 3 takes · 12:40 · about 61 MB" (the chosen takes' lengths and sizes added up; "Merge · 0 takes"
 * with none). */
std::string merge_strip_text(const std::vector<TakeRow> &rows, const std::vector<std::string> &chosen);
/* The merge box's second line: "12:40 · about 61 MB". */
std::string merge_detail_text(double seconds, uint64_t bytes);
/* The progress box's title: "Merging... 37%". */
std::string merging_title(int percent);

/* Bluetooth (T4). The pickers' facts of a device; the recorder's standing notice of the capture state ("Connecting the
 * headset...", "The headset is connected but sends no sound.", ""); the muted line under Monitor (ui::monitor_delay_text of
 * the delay while it runs, the echo wording with notice "delay"; "" otherwise). */
ui::BtDeviceFacts bt_facts(const InputSnap &in);
ui::BtDeviceFacts bt_facts(const OutputSnap &o);
std::string standing_notice_text(const CaptureSnap &c);
std::string monitor_detail_text(const MonitorSnap &m);
/* T9: the chosen Bluetooth headset is there and its microphone is closed on purpose (CaptureSnap::mic_closed: the list, the Edit
 * page, playback): the recorder and the Settings rows keep their normal look (not the no-sound-input state). */
bool mic_held(const Snapshot &s);
/* The call-quality notice (ui::kCallQualityNotice), once per play: fed every snapshot, step() is true once while a take plays
 * through a Bluetooth output at call quality (playback.quality "call") or after the wait for the music mode timed out. After
 * a wait that did reach music mode a "call" reading counts only from kCallQualityGraceMs into the play (the profile shown is
 * the last scan's until the next card reading). Leaving the play (stop) arms it again. */
constexpr int64_t kCallQualityGraceMs = 1500;
struct CallQualityWatch {
    bool in_play = false, fired = false, saw_switch = false;
    int64_t play_ms = -1;
    bool step(const Snapshot &s, int64_t now_ms);
};

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
