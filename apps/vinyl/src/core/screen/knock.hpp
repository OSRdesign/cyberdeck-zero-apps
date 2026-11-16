/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl "Knock-knock" (owner request and decisions of 2026-11-16, the launcher roadmap row "Vinyl: knock-knock"): the screen
 * off while the music plays, to save the battery. Three quick taps on the cover square of Now playing switch the backlight
 * fully off (launcher.sock `screen off`, cp0_app_backlight(0)); while it is off Vinyl ignores every touch and every key but
 * the media and volume keys; three quick taps anywhere bring it back (`screen on`). LVGL-free, tested in
 * tests/test_knock.cpp:
 *
 *   TapSequence     the three quick taps: every press under kPressMs (400 ms), every gap from a release to the next press
 *                   under kGapMs (500 ms), the three within kTotalMs (1200 ms, from the first press to the last release),
 *                   the finger within `slop` px of where it went down; a press outside the zone (arming: anywhere but the
 *                   cover square), a long press, a moved finger or a lost press break the run. The window slides: after
 *                   a slow start the last taps can still make a run (a timeout restarts it, nothing has to be waited for).
 *   knock_key_kind  what a key is while the screen is off: Media (play, pause, next, previous, stop: Vinyl acts), Volume
 *                   (up, down, mute: the launcher's keys, it acts on them whatever the app does), AppVolume (- and =,
 *                   Vinyl's own volume keys: they act, owner decision of 2026-11-16), Other (ignored).
 *   knock_available where the setting is offered: Vinyl has the request, board.state's requests lists `screen`, and the
 *                   board has a touch screen (screen.state touch_slots is not 0; absent = unknown = offered).
 *   ScreenGate      the state: Disabled (the setting Off, or the launcher does not answer `screen` on this board), Ready
 *                   (armed: the taps on the cover count), PendingOff (`screen off` asked: input already ignored), Off. Its
 *                   events give the requests to send; restore() is every way out (exit, hidden, the setting Off, the
 *                   launcher gone): the light back, nothing left ignored.
 *   BacklightWorker the requests on a thread of their own (the client blocks up to its 200 ms timeout: never the LVGL
 *                   thread), in order, their results for the controller's tick.
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vinyl {

/* the notice when the launcher would not switch the screen off (refused, or no service): the screen stays as it is */
constexpr const char *kKnockUnavailable = "Screen off is not available.";

struct KnockRules {
    static constexpr int kTaps = 3;
    static constexpr uint64_t kPressMs = 400; /* a press lasts less than this */
    static constexpr uint64_t kGapMs = 500;   /* a release to the next press: less than this */
    static constexpr uint64_t kTotalMs = 1200; /* the first press to the last release: at most this */
    /* the finger moves at most this many px between its press and its release (the toolkit's tap slop: a sixth of the
     * TARGET token, at least 6 px; 13 px on the deck) */
    static int slop_for_target(int target_px) { return target_px / 6 > 6 ? target_px / 6 : 6; }
};

class TapSequence {
public:
    explicit TapSequence(int slop_px = 13) : slop_(slop_px) {}
    void set_slop(int px) { slop_ = px; }
    /* a finger down at (x, y); in_zone false: it breaks the run (arming: a press outside the cover square) */
    void press(int x, int y, uint64_t now_ms, bool in_zone = true);
    /* the finger moved (optional: the release's own point is checked too) */
    void move(int x, int y);
    /* the finger up: true when this release ends a run of three quick taps (the run is then cleared) */
    bool release(int x, int y, uint64_t now_ms);
    /* the press was lost (the toolkit took it): the run is broken */
    void cancel();
    void reset();
    int taps() const { return n_; } /* the good taps of the run so far */
    bool down() const { return down_; }

private:
    struct Tap {
        uint64_t press = 0, release = 0;
    };
    int slop_;
    Tap run_[KnockRules::kTaps];
    int n_ = 0;
    bool down_ = false, good_ = false;
    int px_ = 0, py_ = 0;
    uint64_t pt_ = 0;
};

enum class KeyKind { Media, Volume, AppVolume, Other };
/* by the evdev code: KEY_PLAYPAUSE, KEY_PLAYCD, KEY_PAUSECD, KEY_NEXTSONG, KEY_PREVIOUSSONG, KEY_STOPCD are Media;
 * KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE are Volume; KEY_MINUS, KEY_EQUAL (Vinyl's - and =) are AppVolume */
KeyKind knock_key_kind(uint32_t evdev);

/* screen.state's touch_slots (APP-CONTRACT section 4: 0 no touch screen, 1 single touch, N slots); -1 when the text is not
 * a v=1 file or has no such key (an older launcher, multitasking off: unknown) */
int screen_touch_slots(const std::string &screen_state_text);
/* the Knock-knock row and the taps are offered: the request at hand, `screen` in requests, and not a board known to have
 * no touch screen (touch_slots 0) */
bool knock_available(bool have_request, bool requests_screen, int touch_slots);

enum class GateState { Disabled, Ready, PendingOff, Off };
const char *gate_state_name(GateState s);

class ScreenGate {
public:
    /* What an event asks the controller to do. */
    struct Out {
        bool send_off = false; /* cp0_app_backlight(0) on the worker, with `tag` (its answer is matched by it) */
        bool send_on = false;  /* cp0_app_backlight(1) (on the worker; at exit at once) */
        bool failed = false;   /* the light could not be switched off: say so ("Screen off is not available.") */
        uint64_t tag = 0;
    };
    /* the board answers `screen` (board.state requests) */
    Out set_available(bool on);
    /* the Knock-knock setting */
    Out set_enabled(bool on);
    bool available() const { return available_; }
    bool enabled() const { return enabled_; }
    GateState state() const { return st_; }

    /* three quick taps on the cover square: Ready -> PendingOff (send_off); anything else: nothing */
    Out arm();
    /* the answer to a request (code: 0 done, 1 refused, -1 no service; tag: the one arm() gave). The answer to `off`:
     * done -> Off; refused or no service -> Ready, failed and send_on (the screen stayed on, the input comes back; `on`
     * in case an earlier `off` did get through). Answers that come late (the screen was woken meanwhile, an older
     * `off`) change nothing. */
    Out result(bool on, int code, uint64_t tag);
    /* three quick taps anywhere while off (or while the request is on its way): -> Ready, send_on */
    Out wake();
    /* every way out (the app quits or is hidden, the setting goes Off, the launcher stops answering `screen`): the light
     * back if Vinyl may have switched it off, then Ready (or Disabled) */
    Out restore();

    bool armable() const { return st_ == GateState::Ready; }
    /* the screen is off or going off: touches only count for the wake taps, keys only the media and volume ones */
    bool input_blocked() const { return st_ == GateState::PendingOff || st_ == GateState::Off; }
    bool accepts_touch() const { return !input_blocked(); }
    bool accepts_key(uint32_t evdev) const { return !input_blocked() || knock_key_kind(evdev) != KeyKind::Other; }

private:
    void settle();
    bool available_ = false, enabled_ = false;
    GateState st_ = GateState::Disabled;
    uint64_t off_tag_ = 0;
};

/* The app's model of it (what the controller holds): the gate and the two tap runs, fed with every touch of the
 * screen. While the screen is on and the gate is Ready, the taps on the cover square arm it (a press elsewhere breaks
 * the run); while it is off (or going off) every touch is swallowed and counts for the wake taps, wherever it lands.
 * Each gate change starts both runs again (the arming taps never count for the wake). */
enum class TouchPhase { Press, Move, Release, Cancel };

class Knock {
public:
    using Out = ScreenGate::Out;
    explicit Knock(int slop_px = 13) : arm_(slop_px), wake_(slop_px) {}
    void set_slop(int px) {
        arm_.set_slop(px);
        wake_.set_slop(px);
    }
    Out set_available(bool on) { return changed(gate_.set_available(on)); }
    Out set_enabled(bool on) { return changed(gate_.set_enabled(on)); }
    /* a touch (on_cover: its press landed on the cover square of Now playing); the run that completes acts */
    Out touch(TouchPhase ph, int x, int y, uint64_t now_ms, bool on_cover);
    Out result(bool on, int code, uint64_t tag) { return changed(gate_.result(on, code, tag)); }
    /* exit, hidden, the launcher gone */
    Out restore() { return changed(gate_.restore()); }
    bool accepts_key(uint32_t evdev) const { return gate_.accepts_key(evdev); }
    bool accepts_touch() const { return gate_.accepts_touch(); }
    bool input_blocked() const { return gate_.input_blocked(); }
    bool armable() const { return gate_.armable(); }
    GateState state() const { return gate_.state(); }
    const ScreenGate &gate() const { return gate_; }
    int arm_taps() const { return arm_.taps(); }
    int wake_taps() const { return wake_.taps(); }

private:
    Out changed(const Out &o);
    ScreenGate gate_;
    TapSequence arm_, wake_;
    GateState seen_ = GateState::Disabled;
};

/* The black cover (owner board finding of 2026-11-16: the Pi 3A+ Waveshare DSI panel only dims at backlight 0, the
 * picture stays faintly visible). While the screen is off the shield is fully opaque black over the whole display, and once
 * that black frame is on the panel the drawing is frozen (the display's invalidation off: no animation, clock or label
 * scroll paints under it); on the way back the drawing is thawed and the whole screen repainted. The view does the LVGL
 * work; this decides when, so that every freeze is matched by exactly one thaw (LVGL counts them, and the lifecycle helper
 * nests its own pair while Vinyl is hidden):
 *   blocked and not shown          show (paint the black cover)
 *   blocked, shown, not frozen     freeze (the black frame flushed first), never in the step that showed it, never hidden
 *   not blocked and shown          hide, thaw if frozen, repaint unless hidden (the lifecycle's show repaints then) */
class BlankCover {
public:
    struct Step {
        bool show = false, hide = false, freeze = false, thaw = false, repaint = false;
        bool any() const { return show || hide || freeze || thaw || repaint; }
    };
    Step update(bool blocked, bool hidden);
    bool shown() const { return shown_; }
    bool frozen() const { return frozen_; }

private:
    bool shown_ = false, frozen_ = false;
};

/* The backlight requests on a worker thread, in order (a newer request replaces one that is still waiting, so a quick
 * off-then-on sends only "on"); the results come back through take(). `call` is cp0_app_backlight on the board (0 done,
 * 1 refused, -1 no service), a fake in the tests and the scenes. */
class BacklightWorker {
public:
    using Call = std::function<int(int on)>;
    struct Result {
        bool on = false;
        int code = -1;
        uint64_t tag = 0;
    };
    explicit BacklightWorker(Call call);
    ~BacklightWorker(); /* stop() */
    BacklightWorker(const BacklightWorker &) = delete;
    BacklightWorker &operator=(const BacklightWorker &) = delete;
    void request(bool on, uint64_t tag = 0);
    bool take(Result &out);
    /* the thread ends after the call it is in (what still waits is dropped); then call_now() is the only way */
    void stop();
    /* a call on this thread (the exit path, after stop()) */
    int call_now(bool on) { return call_ ? call_(on ? 1 : 0) : -1; }
    int calls() const;
    bool idle() const; /* nothing waits and no call is on its way */

private:
    struct Req {
        bool on = false;
        uint64_t tag = 0;
    };
    void run();
    Call call_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Req> q_;
    std::deque<Result> done_;
    bool quit_ = false, busy_ = false;
    int calls_ = 0;
    std::thread th_;
};

}  // namespace vinyl
