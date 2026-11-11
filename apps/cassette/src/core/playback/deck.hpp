/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the five-key state machine of the recording screen (REW, PLAY/PAUSE, FWD, STOP, REC; owner, 2026-11-08).
 * The Deck never touches ALSA or files: it emits commands that the app carries out with the recorder, the Player and the
 * countdown overlay, and it tells the UI which keys are enabled (dimmed otherwise) and which look pressed. LVGL-free.
 *
 * The rules (plan 016, question 1, and the T5 brief):
 *   PLAY/PAUSE  Idle: plays the last take. Playing: pauses / resumes. Recording: pauses / resumes the recording.
 *   REC         starts a take (latched); with a record timer set, starts the countdown instead (the take at zero).
 *               While a take plays, REC stops the playback and then starts the take (or the countdown).
 *   STOP        ends a recording (the take is kept) or a playback; cancels a countdown (nothing written).
 *   REW / FWD   mode "fast scan 5x" (default): held while a take plays (or is paused), the take plays reversed or forward
 *               at 5x with sound; release returns to play, or to pause if it was paused. Mode "jump 5 s": each press
 *               jumps 5 s; held, the jump repeats every kJumpRepeatMs (on TIMER_TICK). Disabled while recording and in
 *               the countdown. With nothing playing they act on the last take (CueLast: loaded, paused at its start;
 *               a release after a scan leaves it paused where the scan ended).
 *   Countdown   STOP, Esc, a tap on the overlay and leaving the app cancel it; at zero the take starts.
 *   DEVICE_LOST recording: the take ends (kept) and is reported; countdown: cancelled and reported; playing or scanning:
 *               paused and reported.
 *   APP_LEAVE   ends a recording (kept), a playback, a countdown (no background recording, D10).
 * Decisions T5 made where the plan is silent (reported to the orchestrator): REC is ignored while recording, paused or
 * counting down (the timer does not restart); PLAY/PAUSE during a scan ends the scan and toggles (a scan from play ends
 * paused, a scan from pause ends playing); a second REW/FWD key while one is held is ignored; Esc is consumed only by the
 * countdown (handle() returns false otherwise so the app can treat it as "back").
 */
#pragma once

#include "countdown.hpp"
#include "fileops.hpp"
#include "player.hpp"

#include <cstdint>

namespace cassette {

enum class DeckState { Idle, Countdown, Recording, RecordingPaused, Playing, PlayingPaused, ScanningForward, ScanningReverse, kCount };
enum class DeckEvent {
    Rec, Stop, PlayPause, RewDown, RewUp, FwdDown, FwdUp, Esc, TapOverlay, TimerTick, TakeEnded, DeviceLost, AppLeave, kCount
};
enum class DeckKey { Rew, PlayPause, Fwd, Stop, Rec, kCount };
enum class RewFwdMode { FastScan, Jump5s };

const char *deck_state_name(DeckState s);
const char *deck_event_name(DeckEvent e);

struct DeckCommand {
    enum Kind {
        StartRecording, PauseRecording, ResumeRecording, StopRecording,
        PlayLast,        /* open the last take and play it from its start */
        CueLast,         /* open the last take, paused at its start (REW/FWD with nothing playing) */
        PausePlayback, ResumePlayback, StopPlayback,
        BeginScan,       /* dir; speed kScanSpeed */
        EndScan,
        Jump,            /* dir; kJumpSeconds */
        StartCountdown,  /* seconds: show the overlay (the Deck runs the count itself) */
        CancelCountdown, /* hide the overlay; nothing was written */
        DeviceLostNotice /* tell the user the card went away (after the commands that handled it) */
    } kind = StartRecording;
    Direction dir = Direction::Forward;
    unsigned seconds = 0;
    bool operator==(const DeckCommand &o) const {
        return kind == o.kind && (kind != BeginScan && kind != Jump ? true : dir == o.dir) &&
               (kind != StartCountdown || seconds == o.seconds);
    }
};

const char *deck_command_name(DeckCommand::Kind k);

class DeckCommands {
public:
    virtual ~DeckCommands() = default;
    virtual void command(const DeckCommand &c) = 0;
};

constexpr uint32_t kJumpRepeatMs = 300;

class Deck {
public:
    Deck(Clock &clock, DeckCommands &out);

    void set_rew_fwd_mode(RewFwdMode m) { mode_ = m; }
    RewFwdMode rew_fwd_mode() const { return mode_; }
    void set_record_timer(RecordTimer t) { timer_ = t; }
    RecordTimer record_timer() const { return timer_; }
    void set_has_last_take(bool v) { has_take_ = v; } /* set by the app (list loaded, take deleted ...) */
    bool has_last_take() const { return has_take_; }

    /* Returns false when the event meant nothing here (Esc outside the countdown: the app's "back"). */
    bool handle(DeckEvent e);

    DeckState state() const { return st_; }
    bool key_enabled(DeckKey k) const;   /* false: dimmed, a press does nothing */
    bool key_active(DeckKey k) const;    /* the key looks pressed: REC latched, REW/FWD held or scanning */
    bool rec_latched() const;
    bool shows_pause_icon() const;       /* PLAY/PAUSE shows the pause icon while anything runs */
    bool scan_returns_to_pause() const { return resume_paused_; }
    const Countdown &countdown() const { return countdown_; }

private:
    void emit(DeckCommand::Kind k, Direction d = Direction::Forward, unsigned s = 0);
    void start_take_or_countdown();
    void key_down(Direction d);
    void key_up(Direction d);
    void clear_hold() { hold_ = false; }

    Clock &clock_;
    DeckCommands &out_;
    Countdown countdown_;
    DeckState st_ = DeckState::Idle;
    RewFwdMode mode_ = RewFwdMode::FastScan;
    RecordTimer timer_ = RecordTimer::Off;
    bool has_take_ = false;
    bool resume_paused_ = false; /* the state a scan returns to */
    bool hold_ = false;          /* jump mode: a REW/FWD key is held */
    Direction hold_dir_ = Direction::Forward;
    uint64_t last_jump_ms_ = 0;
};

} // namespace cassette
