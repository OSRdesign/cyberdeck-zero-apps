/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app: the UI-agnostic application controller (task T6a). It wires the finished core units into the real
 * pipelines and gives the UI two things: commands (key events, settings, take operations, the trim page) and
 * snapshot() (a value struct the UI polls at 30 Hz). No LVGL, no UI types: every struct below is plain ints, floats,
 * strings and vectors, so the UI side can mirror them one to one.
 *
 * Recording pipeline (P0 results, study 058):
 *   capture thread (Capture, 1024-frame periods) -> ProcessingSink (DC blocker, SOFT gain, optional 80 Hz high-pass and
 *   limiter; the meter taps the processed signal) -> RecordGate -> Ring (8 s) -> Writer thread (BWF header, sync every N s,
 *   auto-split with _pN names, journal).
 *   The capture path is opened at start() and keeps running, so the meter is live on the idle screen. While no take
 *   runs the gate discards the audio: nothing is written, nothing is buffered, the only cost is the DC blocker, the gain and
 *   the meter on one period at a time (48 kHz mono: well under 1 % of a Pi Zero 2 W core). REC creates a fresh Ring and
 *   Writer and opens the gate; STOP closes the gate (waiting for a write in flight), then stops the Writer (the ring is
 *   drained, the header patched, the journal closed).
 *
 * Thread model:
 *   - UI thread: calls commands and snapshot()/takes()/peak_window(). Commands take one internal mutex `mu_`; a command
 *     that talks to the Player may wait for the playback thread (at most kCommandWaitMs, normally a few ms). snapshot(),
 *     takes() and peak_window() never take `mu_`: they copy the last published state (a separate small mutex) so the 30 Hz
 *     poll is never held up by a command. Every command republishes the snapshot before it returns, so the UI sees its own
 *     effect on the next poll.
 *   - service thread (started by start(), one per Controller, every service_period_ms): feeds the Deck its timer tick
 *     (countdown, jump repeat), carries Player events (take ended, device lost) into the Deck, notices a lost card or a
 *     failed Writer, retries a lost card once a second, follows the preview, and publishes the snapshot.
 *   - capture thread (Capture) and writer thread (Writer): no controller locks; they talk through atomics, the Ring and
 *     the gate. The meter is read lock-free.
 *   - workers: one thread builds the peak pyramid (open_trim), one runs save() (trim_split). They touch only atomics and a
 *     small result mutex; the service thread merges their results into the snapshot. app_leave()/the destructor cancel and
 *     join them.
 *   Lock order: mu_ -> (result mutex | publish mutex | peak-pointer mutex). A command never waits for a worker.
 *
 * Decisions the plan did not fix (also listed in the T6a report):
 *   - the target of PLAY/REW/FWD is the "current take": the newest take at start, the one just recorded, or the one the
 *     user selected (select_take / play_take); deleting it moves the target to the newest take.
 *   - wind mode and low cut are the same switch (the 80 Hz high-pass, D18); set_wind_mode() is an alias.
 *   - only WAV is implemented: set_format() refuses Flac and Opus (returns false).
 *   - changing the folder is refused while a take or countdown runs.
 *   - trim: moving IN or OUT removes the splits that fall outside (IN, OUT); the marker is the only thing the UI moves
 *     (set_in/set_out/add_split take the marker position); preview plays [IN, OUT) from the marker when it is inside, else
 *     from IN, and stops at OUT with the marker left there.
 *   - save(ReplaceOriginal) with several parts removes the original; the journal of the original stays as the take's record.
 */
#pragma once

#include "../alsa/alsa_api.hpp"
#include "../fileops.hpp"
#include "../frame_sink.hpp"
#include "../takes/naming.hpp"
#include "../takes/settings.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cassette {

class PlaybackSink;

/* ---- construction ---- */

struct ControllerConfig {
    AlsaApi *api = nullptr;          /* required: the sound system (the real one, or a fake in tests) */
    FileOps *ops = nullptr;          /* null: posix_file_ops() */
    Clock *clock = nullptr;          /* null: system_clock() */
    ClockState *clock_state = nullptr; /* has the wall clock been set? null: the timesyncd marker */
    std::string home;                /* "~" in paths, and the default settings file <home>/.config/cassette/settings.json */
    std::string settings_path;       /* empty: default_settings_path(home) */
    /* The playback output for a device string ("hw:CARD=X,DEV=0" or "default"). Empty: AlsaPlaybackSink on `api`. */
    std::function<std::unique_ptr<PlaybackSink>(const std::string &device)> make_sink;
    uint64_t min_free_bytes = 200ull << 20; /* a take stops cleanly under this much free space */
    uint32_t writer_sync_ms = 0;     /* when not 0, overrides the sync interval setting (tests) */
    uint32_t service_period_ms = 20;
    uint32_t replug_retry_ms = 1000; /* how often a lost card is looked for */
    size_t save_block_bytes = 256 * 1024; /* copy block of the trim engine (tests use small ones) */
    uint64_t split_bytes = 0;        /* when not 0, overrides the writer's 4 GiB part size (tests) */
};

/* ---- keys ---- */

enum class KeyEvent { Rec, Stop, PlayPause, RewDown, RewUp, FwdDown, FwdUp, Esc, TapOverlay, AppLeave };

/* ---- snapshot structs ---- */

/* Deck state codes (DeckState as an int) */
enum : int {
    kDeckIdle = 0, kDeckCountdown, kDeckRecording, kDeckRecordingPaused, kDeckPlaying, kDeckPlayingPaused,
    kDeckScanningForward, kDeckScanningReverse
};

struct KeySnap {
    bool enabled = false; /* false: dimmed, a press does nothing */
    bool active = false;  /* looks pressed: REC latched, REW/FWD held or scanning */
};

struct DeckSnap {
    int state = kDeckIdle;
    std::string state_name = "Idle";
    KeySnap rew, play_pause, fwd, stop, rec;
    bool rec_latched = false;
    bool pause_icon = false;  /* PLAY/PAUSE shows the pause icon */
};

struct TimeSnap {
    double elapsed_s = 0;       /* recording: length of the take so far (frozen while paused) */
    double position_s = 0;      /* playing, paused or scanning: the position in the take */
    double length_s = 0;        /* ... and its length */
    int countdown_seconds = 0;  /* record timer: 3, 2, 1; 0 when not counting */
    float countdown_fraction = 0.f; /* 1.0 at the start down to 0.0 at zero */
};

struct MeterChannelSnap {
    float peak_db = -120.f, rms_db = -120.f, hold_db = -120.f;
    int bar = 0;        /* lit steps 0..15 */
    int hold_bar = 0;
    bool peak_lamp = false;
    int zone = 0;       /* 0 low, 1 in the target zone (-18..-6 dBFS), 2 high */
};

struct MeterSnap {
    int channels = 0;   /* 0 before the first period, then 1 (mono) or 2 (stereo) */
    MeterChannelSnap ch[2];
    unsigned clips = 0; /* samples saturated by SOFT gain since the pipeline opened */
};

struct CardSnap {
    bool present = false;      /* the capture path is open and delivering */
    std::string label;         /* the chosen card's label, empty when none */
    std::vector<std::string> labels; /* every capture card seen at the last refresh (distinct labels) */
    int selected = -1;         /* index into labels */
    bool has_level = false, has_agc = false, has_mute = false;
    double level_db = 0, level_min_db = 0, level_max_db = 0;
    bool level_inferred = false; /* the dB range is the 0..23.81 assumption, not the driver's */
    bool auto_gain = false, mute = false;
    double soft_gain_db = 0;
    bool via_server = false;   /* recording through the sound server (the card is held by another program) */
    std::string message;       /* why the card cannot be used, empty when fine */
    std::string output_label;  /* the playback output in use */
    std::vector<std::string> output_labels; /* only when there is more than one to pick from */
    std::vector<std::string> output_devices; /* the device of each label (select_output's argument), same order */
    int output_selected = -1;  /* index of the output in use in output_labels, -1 when not listed */
};

struct TakeSnap {
    std::string name;          /* file name of the current take (recording: the new take) */
    std::string format_text;   /* "48k/16 WAV", "44.1k/16 stereo WAV" */
    uint64_t free_bytes = 0;
    double hours_left = 0;     /* at the current format */
    bool recording = false;
};

struct ProblemSnap {
    uint64_t frames_dropped = 0; /* the ring was full */
    uint64_t frames_lost = 0;    /* estimate of the audio lost to capture overruns */
    unsigned overruns = 0;
    bool unplugged = false;      /* the last take ended because the card went away */
    bool disk_full = false;      /* ... because the disk filled up (or the free space got too low) */
    bool write_error = false;    /* ... because of another write error */
    std::string message;         /* the last message text, empty when none */
};

struct Notice {
    enum Kind { Repaired = 1, CardLost = 2, CardBack = 3, Info = 4 };
    int kind = Info;
    std::string text;
};

struct ScanSnap {
    bool active = false;
    int direction = 0;           /* -1 reverse, +1 forward, 0 not scanning */
    std::string speed_text;      /* "5x" while scanning, else empty */
};

/* The trim page (open_trim ... close_trim). Positions are frames of the take. */
struct TrimSnap {
    bool open = false;
    std::string take;            /* file name */
    uint64_t frames = 0;
    uint32_t rate = 48000;
    uint64_t in_frame = 0, out_frame = 0; /* kept part [in, out), out exclusive */
    std::vector<uint64_t> splits;         /* sorted, inside (in, out) */
    int selected_split = -1;
    uint64_t position = 0;       /* the marker */
    int zoom = 0;                /* 0 = whole take */
    int max_zoom = 0;
    uint64_t window_start = 0;   /* first frame of the visible window */
    uint64_t visible_frames = 0; /* frames the window shows (peak_window() spreads them over its bars) */
    int peaks_state = 0;         /* 0 none, 1 building, 2 ready, 3 failed */
    float peaks_progress = 0.f;  /* 0..1 while building */
    bool peaks_in_memory = false; /* the cache file could not be written: the pyramid is held in memory */
    std::string peaks_error;
    bool previewing = false;
    double kept_seconds = 0;     /* out - in */
    int save_state = 0;          /* 0 none, 1 running, 2 done, 3 failed, 4 cancelled */
    float save_progress = 0.f;
    std::string save_message;
    std::vector<std::string> save_outputs; /* file names, in part order, once done */
    bool save_replaced = false;  /* the last save replaced the original */
};

struct Snapshot {
    bool started = false;
    DeckSnap deck;
    TimeSnap time;
    MeterSnap meter;
    CardSnap card;
    TakeSnap take;
    ProblemSnap problems;
    std::vector<Notice> notices; /* one-time: shown once, then ack_notices() */
    ScanSnap scan;
    TrimSnap trim;
    unsigned takes_revision = 0; /* changes whenever takes() would return something different */
    std::string folder;          /* the recordings folder, "~" expanded */
    Settings settings;           /* the current settings */
};

struct TakeRow {
    std::string name, label, path;
    double seconds = 0;
    uint64_t frames = 0, size = 0;
    uint32_t rate = 0;
    int channels = 0, bits = 0;
    std::string format_text;
    std::string date_text;       /* "2026-11-08 11:05", empty when not set */
    bool damaged = false, repaired = false;
    bool current = false;        /* the take PLAY, REW and FWD act on */
};

struct PeakBar { int min = 0, max = 0; }; /* 16-bit scale */

enum class SaveKind { NewFiles, ReplaceOriginal };

/* ---- the controller ---- */

class Controller {
public:
    explicit Controller(ControllerConfig cfg);
    ~Controller(); /* shutdown() */
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    /* Startup (blocks for the repair and the card enumeration, tens of ms: call it before the first frame or from a
     * worker): loads the settings, repairs the recordings folder (the repaired takes become notices), lists the capture
     * cards, selects the saved card by descriptor (never by index) else the first one, re-applies its saved mixer state,
     * opens the capture path and lets the meter run, chooses the playback output, starts the service thread. */
    void start();
    /* Ends a running take cleanly, cancels a save, stops every thread. Idempotent. */
    void shutdown();

    /* ---- polled state ---- */
    Snapshot snapshot() const;
    std::vector<TakeRow> takes() const;
    /* The trim page's waveform: `bars` min/max pairs over [window_start, window_start + visible_frames); empty until the
     * peaks are ready. Does one pread; safe to call from the UI thread at any rate. */
    std::vector<PeakBar> peak_window(int bars) const;
    /* The same over any range (task T6c: the UI draws a window centred on the marker): `bars` pairs over
     * [start_frame, start_frame + frames); start_frame may be negative, bars outside the take are (0,0). Empty until the
     * peaks are ready or when the trim page is closed. */
    std::vector<PeakBar> peak_range(int64_t start_frame, uint64_t frames, int bars) const;
    /* Clears the notices the UI has shown. */
    void ack_notices();

    /* ---- keys: routed to the Deck, whose commands the controller carries out. Returns false when the event meant
     * nothing (Esc outside a countdown: the UI's "back"). AppLeave also cancels a save and closes the trim page. */
    bool key_event(KeyEvent e);
    void app_leave() { key_event(KeyEvent::AppLeave); }

    /* ---- settings (applied live where possible, persisted at once). false: refused (bad value, busy). ---- */
    bool set_level_db(double db);          /* the card's capture level; nearest step; false when the card has none */
    bool set_auto_gain(bool on);
    bool set_mute(bool on);
    bool set_soft_gain_db(double db);      /* 0..24 */
    bool set_format(RecFormat f);          /* only Wav */
    bool set_folder(const std::string &folder);
    bool set_prefix(const std::string &prefix);
    bool set_sync_interval(int seconds);   /* 5..30 */
    bool set_auto_split(bool on);
    bool set_record_timer(int seconds);    /* 0, 3, 10 or 20 */
    bool set_scan_mode(ScanMode m);
    bool set_wind_mode(bool on) { return set_low_cut(on); }
    bool set_low_cut(bool on);             /* 80 Hz high-pass */
    bool set_limiter(bool on);
    bool select_card(int index);           /* index into snapshot().card.labels; refused while a take runs */
    bool select_output(const std::string &device); /* "" for automatic */
    void refresh_cards();                  /* re-lists the capture cards (the labels), e.g. on opening Settings */

    /* ---- takes ---- */
    void refresh_takes();
    bool select_take(const std::string &name);   /* makes it the current take */
    bool play_take(const std::string &name);     /* select + play from the start */
    int rename_take(const std::string &name, const std::string &new_prefix, std::string *new_name = nullptr); /* 0 or -errno */
    int delete_take(const std::string &name);    /* 0 or -errno; -EBUSY while it plays or is open for trimming */

    /* ---- trim page ---- */
    bool open_trim(const std::string &take_name); /* quick: reads the header; the peaks build on a worker */
    void close_trim();                            /* cancels a build, a preview and a save */
    bool set_position(uint64_t frame);
    bool move_position(int64_t delta_frames);
    bool set_in();                                /* IN = marker; refused when not before OUT */
    bool set_out();                               /* OUT = marker (exclusive); refused when not after IN */
    bool add_split();                             /* a split at the marker, inside (IN, OUT) */
    bool remove_split(int n);
    bool select_split(int n);                     /* moves the marker onto split n */
    bool set_zoom(int level);
    bool preview(bool on);                        /* plays the kept part; only with the deck idle */
    bool save(SaveKind kind);                     /* runs on a worker; false when invalid or already saving */
    void cancel_save();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cassette
