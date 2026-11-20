/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl "Knock-knock" (see knock.hpp).
 */
#include "knock.hpp"

#include "../launcher/launcher_link.hpp"
#include "../pulse/launcher_facts.hpp"

#include <cstdlib>

namespace vinyl {

/* ------------------------------------------------------------------------------------------ the taps */

void TapSequence::reset() {
    n_ = 0;
    down_ = false;
    good_ = false;
}

void TapSequence::press(int x, int y, uint64_t now_ms, bool in_zone) {
    if (down_) n_ = 0; /* a press without a release before it (a lost one): the run starts again */
    down_ = true;
    good_ = in_zone;
    px_ = x;
    py_ = y;
    pt_ = now_ms;
    if (!in_zone) { /* a press elsewhere breaks the run */
        n_ = 0;
        return;
    }
    /* a gap too long (or a clock that went back): the taps before it no longer count */
    if (n_ > 0) {
        const uint64_t last = run_[n_ - 1].release;
        if (now_ms < last || now_ms - last >= KnockRules::kGapMs) n_ = 0;
    }
}

void TapSequence::move(int x, int y) {
    if (down_ && good_ && (std::abs(x - px_) > slop_ || std::abs(y - py_) > slop_)) good_ = false;
}

bool TapSequence::release(int x, int y, uint64_t now_ms) {
    if (!down_) return false;
    move(x, y);
    down_ = false;
    if (!good_ || now_ms < pt_ || now_ms - pt_ >= KnockRules::kPressMs) { /* moved, elsewhere, or a long press */
        n_ = 0;
        return false;
    }
    run_[n_++] = Tap{pt_, now_ms};
    /* the window slides: the first taps go while the run is longer than the total */
    while (n_ > 0 && now_ms - run_[0].press > KnockRules::kTotalMs) {
        for (int i = 1; i < n_; ++i) run_[i - 1] = run_[i];
        --n_;
    }
    if (n_ < KnockRules::kTaps) return false;
    n_ = 0;
    return true;
}

void TapSequence::cancel() {
    down_ = false;
    good_ = false;
    n_ = 0;
}

/* ------------------------------------------------------------------------------------------ the keys */

KeyKind knock_key_kind(uint32_t evdev) {
    switch (evdev) {
    case 163: /* KEY_NEXTSONG */
    case 164: /* KEY_PLAYPAUSE */
    case 165: /* KEY_PREVIOUSSONG */
    case 166: /* KEY_STOPCD */
    case 200: /* KEY_PLAYCD */
    case 201: /* KEY_PAUSECD */
        return KeyKind::Media;
    case 113: /* KEY_MUTE */
    case 114: /* KEY_VOLUMEDOWN */
    case 115: /* KEY_VOLUMEUP */
        return KeyKind::Volume;
    case 12: /* KEY_MINUS: Vinyl's volume down */
    case 13: /* KEY_EQUAL: Vinyl's volume up */
        return KeyKind::AppVolume;
    default: return KeyKind::Other;
    }
}

/* ------------------------------------------------------------------------------------------ where it is offered */

int screen_touch_slots(const std::string &screen_state_text) {
    const StateText st = parse_state_text(screen_state_text);
    if (!st.ok) return -1;
    auto it = st.kv.find("touch_slots");
    if (it == st.kv.end() || it->second.empty()) return -1;
    char *end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (!end || *end != 0 || v < 0 || v > 1000) return -1;
    return static_cast<int>(v);
}

bool knock_available(bool have_request, bool requests_screen, int touch_slots) {
    return have_request && requests_screen && touch_slots != 0;
}

/* ------------------------------------------------------------------------------------------ the gate */

const char *gate_state_name(GateState s) {
    switch (s) {
    case GateState::Ready: return "ready";
    case GateState::PendingOff: return "going off";
    case GateState::Off: return "off";
    default: return "disabled";
    }
}

void ScreenGate::settle() {
    const bool active = available_ && enabled_;
    if (!active && st_ == GateState::Ready) st_ = GateState::Disabled;
    else if (active && st_ == GateState::Disabled) st_ = GateState::Ready;
}

ScreenGate::Out ScreenGate::set_available(bool on) {
    if (available_ == on) return Out{};
    available_ = on;
    Out o = on ? Out{} : restore();
    settle();
    return o;
}

ScreenGate::Out ScreenGate::set_enabled(bool on) {
    if (enabled_ == on) return Out{};
    enabled_ = on;
    Out o = on ? Out{} : restore();
    settle();
    return o;
}

ScreenGate::Out ScreenGate::arm() {
    Out o;
    if (st_ != GateState::Ready) return o;
    st_ = GateState::PendingOff;
    ++off_tag_;
    o.send_off = true;
    o.tag = off_tag_;
    return o;
}

ScreenGate::Out ScreenGate::result(bool on, int code, uint64_t tag) {
    Out o;
    if (on || st_ != GateState::PendingOff || tag != off_tag_) return o; /* an answer to `on`, or a late one */
    if (code == 0) {
        st_ = GateState::Off;
        return o;
    }
    /* refused or no service: the screen stayed on; the input comes back. `on` after it, in case an earlier `off` of this
     * run did get through */
    st_ = available_ && enabled_ ? GateState::Ready : GateState::Disabled;
    o.failed = true;
    o.send_on = true;
    return o;
}

ScreenGate::Out ScreenGate::wake() {
    Out o;
    if (!input_blocked()) return o;
    st_ = available_ && enabled_ ? GateState::Ready : GateState::Disabled;
    o.send_on = true;
    return o;
}

ScreenGate::Out ScreenGate::restore() {
    Out o;
    o.send_on = input_blocked();
    st_ = available_ && enabled_ ? GateState::Ready : GateState::Disabled;
    return o;
}

/* ------------------------------------------------------------------------------------------ the app's model */

ScreenGate::Out Knock::changed(const ScreenGate::Out &o) {
    if (gate_.state() != seen_) { /* a new state: both runs start again */
        seen_ = gate_.state();
        arm_.reset();
        wake_.reset();
    }
    return o;
}

ScreenGate::Out Knock::touch(TouchPhase ph, int x, int y, uint64_t now_ms, bool on_cover) {
    const bool blocked = gate_.input_blocked();
    if (!blocked && !gate_.armable()) { /* Disabled: nothing counts */
        arm_.reset();
        wake_.reset();
        return Out{};
    }
    TapSequence &seq = blocked ? wake_ : arm_;
    bool done = false;
    switch (ph) {
    case TouchPhase::Press: seq.press(x, y, now_ms, blocked || on_cover); break; /* waking: anywhere */
    case TouchPhase::Move: seq.move(x, y); break;
    case TouchPhase::Release: done = seq.release(x, y, now_ms); break;
    case TouchPhase::Cancel: seq.cancel(); break;
    }
    if (!done) return Out{};
    return changed(blocked ? gate_.wake() : gate_.arm());
}

/* ------------------------------------------------------------------------------------------ the black cover */

BlankCover::Step BlankCover::update(bool blocked, bool hidden) {
    Step s;
    if (blocked) {
        if (!shown_) {
            shown_ = true;
            s.show = true;
        } else if (!frozen_ && !hidden) {
            frozen_ = true;
            s.freeze = true;
        }
        return s;
    }
    if (shown_) {
        shown_ = false;
        s.hide = true;
        if (frozen_) {
            frozen_ = false;
            s.thaw = true;
        }
        s.repaint = !hidden;
    }
    return s;
}

/* ------------------------------------------------------------------------------------------ the request */

int screen_call(const std::string &socket_path, bool on, int timeout_ms) {
    return launcher_call(socket_path, on ? "screen on" : "screen off", nullptr, timeout_ms);
}

int launcher_screen_call(int on) { return screen_call(launcher_socket_path(), on != 0); }

/* ------------------------------------------------------------------------------------------ the worker */

BacklightWorker::BacklightWorker(Call call) : call_(std::move(call)) {
    th_ = std::thread([this] { run(); });
}

BacklightWorker::~BacklightWorker() { stop(); }

void BacklightWorker::request(bool on, uint64_t tag) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_) return;
        q_.clear(); /* only the last wish counts: an off still waiting is replaced by the on after it */
        q_.push_back(Req{on, tag});
    }
    cv_.notify_all();
}

bool BacklightWorker::take(Result &out) {
    std::lock_guard<std::mutex> g(mu_);
    if (done_.empty()) return false;
    out = done_.front();
    done_.pop_front();
    return true;
}

int BacklightWorker::calls() const {
    std::lock_guard<std::mutex> g(mu_);
    return calls_;
}

bool BacklightWorker::idle() const {
    std::lock_guard<std::mutex> g(mu_);
    return q_.empty() && !busy_;
}

void BacklightWorker::stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
        q_.clear();
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void BacklightWorker::run() {
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
            if (quit_) return;
            r = q_.front();
            q_.pop_front();
            busy_ = true;
        }
        const int code = call_ ? call_(r.on ? 1 : 0) : -1;
        {
            std::lock_guard<std::mutex> g(mu_);
            ++calls_;
            busy_ = false;
            done_.push_back(Result{r.on, code, r.tag});
        }
    }
}

}  // namespace vinyl
