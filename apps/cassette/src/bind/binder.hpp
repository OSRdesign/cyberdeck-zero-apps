/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding (task T6c): the UI's actions to controller commands, the controller's state to the UI's model.
 * No LVGL: the glue (bound_app.cpp) calls tick() from an LVGL timer every 33 ms and hands vm() to CassetteUi::update();
 * the tests (tests/bind) drive it with a fake Port and an inline Worker.
 *
 * Threads: actions() and tick() run on the UI thread and never wait: every command is posted to the Worker (in order);
 * tick() reads only the controller's non-blocking snapshot(), takes() (when its revision moved) and peak_range() (when
 * the trim window moved). What a job finds out for the UI (notices, the next take number, the names a save would
 * write, a page to go back to) comes back through a small locked inbox that tick() empties.
 *
 * Actions to commands:
 *   deck keys      REW / FWD Down and Up -> RewDown/RewUp, FwdDown/FwdUp; PLAY/PAUSE, STOP, REC on Click (the UI's rule)
 *   countdown      a tap on the panel or Esc -> TapOverlay (cancels; nothing outside a countdown)
 *   CARD / SOFT    one step (1 dB; CARD: a larger step when the card's own step is coarser) from the controller's value
 *                  at the time the job runs, so quick taps add up
 *   settings       input -> select_card, level -> set_level_db, agc -> set_auto_gain, format -> set_format(WAV),
 *                  timer -> set_record_timer, rewfwd -> set_scan_mode, lowcut / limiter, folder -> set_folder, prefix ->
 *                  set_prefix (a refused prefix gives a notice), split -> set_auto_split; channels: nothing
 *   pages          Settings -> refresh_cards; List -> refresh_takes and the highlight back on the current take; leaving
 *                  the trim page stops the preview and closes it (after a running save has finished)
 *   list           select -> the highlight only (nothing is loaded); activate (Enter, a tap on the highlighted row) ->
 *                  preview_take, or Stop (stop and rewind) when that take is the one playing; LOAD -> load (refused while
 *                  recording); leaving the list stops the preview it started; RENAME -> rename_take; TRIM -> stop a
 *                  playback, open_trim (refused: back to the list); DELETE -> the confirm box, then delete_take
 *   trim           ZOOM -> set_zoom (down to a 2 s window), POSITION / keys -> move_position, a drag -> set_position
 *                  (coalesced), SET IN / SET OUT / SPLIT / REMOVE / a flag tap -> set_in / set_out / add_split /
 *                  remove_split / select_split, PLAY -> preview(toggle), SAVE -> the save box with the names a save would
 *                  write; SAVE AS NEW FILES -> save(NewFiles); REPLACE ORIGINAL -> the confirm box, then
 *                  save(ReplaceOriginal); CANCEL closes the box
 *   app exit       leave(): the queued commands run, then app_leave() and shutdown() (the take is closed and kept, a
 *                  save is cancelled, the journal updated)
 * Notices (one-time controller notices, the binding's own): shown one after the other in the recorder's strip, each for
 * Options::notice_ms.
 */
#pragma once

#include "port.hpp"
#include "view_map.hpp"
#include "worker.hpp"

#include "ui/view_model.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cassette {
namespace bind {

class Binder {
public:
    struct Options {
        int64_t notice_ms = 4000;
        int window_bars = 220;   /* the UI resamples them to its bar count */
        int overview_bars = 110;
        double card_step_db = 1.0;
        double soft_step_db = 1.0;
        double min_window_s = 2.0;
        bool utc = false;        /* tests */
    };

    Binder(Port &port, Worker &worker, Options opt);
    Binder(Port &port, Worker &worker) : Binder(port, worker, Options()) {}
    ~Binder(); /* the worker finishes first: no job outlives the binder */
    Binder(const Binder &) = delete;
    Binder &operator=(const Binder &) = delete;

    ui::UiActions actions();
    /* The 30 Hz poll (UI thread). True when the model changed since the last tick. */
    bool tick(int64_t now_mono_ms, int64_t now_wall_s);
    const ui::ViewModel &vm() const { return vm_; }
    const LocalState &local() const { return local_; }
    /* A page the binding asks the UI to show (TRIM on a take that could not be opened: back to the list). */
    bool take_nav(ui::Screen &screen);
    /* A notice of the binding's own (UI thread). */
    void push_notice(const std::string &text);
    /* The app ends: the queued commands run, then app_leave() and shutdown(). Blocks; idempotent. */
    void leave();

private:
    struct Inbox {
        std::mutex mu;
        std::vector<std::string> notices;
        bool notices_done = false;
        bool have_next = false;
        uint64_t next_take = 0;
        bool have_names = false;
        unsigned names_token = 0;
        std::vector<std::string> names;
        bool have_nav = false;
        ui::Screen nav = ui::Screen::List;
    };
    void post(std::function<void()> job) { worker_.post(std::move(job)); }
    void notice_from_job(const std::string &text);
    void drain_inbox();
    void leave_trim();
    std::string take_at(int index) const;
    void update_peaks();

    Port &port_;
    Worker &worker_;
    Options opt_;
    Inbox inbox_;
    bool left_ = false;
    bool list_preview_ = false; /* the list started a preview that closing it must stop */

    /* UI thread */
    Snapshot snap_;
    bool have_snap_ = false;
    std::vector<TakeRow> rows_;
    unsigned rows_rev_ = 0;
    bool have_rows_ = false;
    std::string next_folder_;
    LocalState local_;
    ui::ViewModel vm_;
    bool have_vm_ = false;
    ui::Screen screen_ = ui::Screen::Recorder;
    std::deque<std::string> notices_;
    int64_t notice_since_ = 0, now_ms_ = 0;
    bool notice_job_ = false;
    bool pending_close_ = false;
    std::string pending_open_;
    unsigned names_token_ = 0;
    std::vector<int64_t> names_lengths_;
    int last_save_state_ = 0;
    std::atomic<bool> save_accepted_{false}; /* set by the job when the controller took a save */
    bool save_wait_ = false;                 /* a save was taken and its end has not been seen yet */
    bool nav_pending_ = false;
    ui::Screen nav_ = ui::Screen::List;
    std::string select_after_nav_; /* the list's selection when the page opens by itself (after a replace save) */
    /* the trim waveform: re-read only when the window moved */
    std::vector<PeakBar> window_, overview_;
    std::string peaks_key_, overview_key_;
    /* a drag on the waveform: only the latest marker is sent */
    std::atomic<int64_t> scrub_frame_{0};
    std::atomic<bool> scrub_posted_{false};
};

} // namespace bind
} // namespace cassette
