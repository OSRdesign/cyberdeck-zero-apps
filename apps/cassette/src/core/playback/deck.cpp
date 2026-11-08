/* SPDX-License-Identifier: MIT */
#include "deck.hpp"

namespace cassette {

const char *deck_state_name(DeckState s) {
    switch (s) {
    case DeckState::Idle: return "Idle";
    case DeckState::Countdown: return "Countdown";
    case DeckState::Recording: return "Recording";
    case DeckState::RecordingPaused: return "RecordingPaused";
    case DeckState::Playing: return "Playing";
    case DeckState::PlayingPaused: return "PlayingPaused";
    case DeckState::ScanningForward: return "ScanningForward";
    case DeckState::ScanningReverse: return "ScanningReverse";
    case DeckState::kCount: break;
    }
    return "?";
}

const char *deck_event_name(DeckEvent e) {
    switch (e) {
    case DeckEvent::Rec: return "REC";
    case DeckEvent::Stop: return "STOP";
    case DeckEvent::PlayPause: return "PLAY_PAUSE";
    case DeckEvent::RewDown: return "REW_DOWN";
    case DeckEvent::RewUp: return "REW_UP";
    case DeckEvent::FwdDown: return "FWD_DOWN";
    case DeckEvent::FwdUp: return "FWD_UP";
    case DeckEvent::Esc: return "ESC";
    case DeckEvent::TapOverlay: return "TAP_OVERLAY";
    case DeckEvent::TimerTick: return "TIMER_TICK";
    case DeckEvent::TakeEnded: return "TAKE_ENDED";
    case DeckEvent::DeviceLost: return "DEVICE_LOST";
    case DeckEvent::AppLeave: return "APP_LEAVE";
    case DeckEvent::kCount: break;
    }
    return "?";
}

const char *deck_command_name(DeckCommand::Kind k) {
    switch (k) {
    case DeckCommand::StartRecording: return "StartRecording";
    case DeckCommand::PauseRecording: return "PauseRecording";
    case DeckCommand::ResumeRecording: return "ResumeRecording";
    case DeckCommand::StopRecording: return "StopRecording";
    case DeckCommand::PlayLast: return "PlayLast";
    case DeckCommand::CueLast: return "CueLast";
    case DeckCommand::PausePlayback: return "PausePlayback";
    case DeckCommand::ResumePlayback: return "ResumePlayback";
    case DeckCommand::StopPlayback: return "StopPlayback";
    case DeckCommand::BeginScan: return "BeginScan";
    case DeckCommand::EndScan: return "EndScan";
    case DeckCommand::Jump: return "Jump";
    case DeckCommand::StartCountdown: return "StartCountdown";
    case DeckCommand::CancelCountdown: return "CancelCountdown";
    case DeckCommand::DeviceLostNotice: return "DeviceLostNotice";
    }
    return "?";
}

Deck::Deck(Clock &clock, DeckCommands &out) : clock_(clock), out_(out), countdown_(clock) {}

void Deck::emit(DeckCommand::Kind k, Direction d, unsigned s) {
    DeckCommand c;
    c.kind = k;
    c.dir = d;
    c.seconds = s;
    out_.command(c);
}

void Deck::start_take_or_countdown() {
    const unsigned n = timer_seconds(timer_);
    if (n > 0 && countdown_.start(n)) {
        emit(DeckCommand::StartCountdown, Direction::Forward, n);
        st_ = DeckState::Countdown;
    } else {
        emit(DeckCommand::StartRecording);
        st_ = DeckState::Recording;
    }
}

/* REW/FWD pressed in Idle, Playing or PlayingPaused (the callers check). */
void Deck::key_down(Direction d) {
    if (hold_) return; /* the other key is held: one at a time */
    const bool idle = st_ == DeckState::Idle;
    if (idle) emit(DeckCommand::CueLast);
    if (mode_ == RewFwdMode::FastScan) {
        resume_paused_ = st_ != DeckState::Playing;
        emit(DeckCommand::BeginScan, d);
        st_ = d == Direction::Forward ? DeckState::ScanningForward : DeckState::ScanningReverse;
    } else {
        emit(DeckCommand::Jump, d);
        hold_ = true;
        hold_dir_ = d;
        last_jump_ms_ = clock_.mono_ms();
        if (idle) st_ = DeckState::PlayingPaused;
    }
}

void Deck::key_up(Direction d) {
    if (hold_ && hold_dir_ == d) hold_ = false;
}

bool Deck::handle(DeckEvent e) {
    using C = DeckCommand;
    switch (st_) {
    case DeckState::Idle:
        switch (e) {
        case DeckEvent::Rec: start_take_or_countdown(); return true;
        case DeckEvent::PlayPause:
            if (!has_take_) return false;
            emit(C::PlayLast);
            st_ = DeckState::Playing;
            return true;
        case DeckEvent::RewDown:
        case DeckEvent::FwdDown:
            if (!has_take_) return false;
            key_down(e == DeckEvent::RewDown ? Direction::Reverse : Direction::Forward);
            return true;
        default: return false;
        }

    case DeckState::Countdown:
        switch (e) {
        case DeckEvent::Stop:
        case DeckEvent::Esc:
        case DeckEvent::TapOverlay:
        case DeckEvent::AppLeave:
            countdown_.cancel();
            emit(C::CancelCountdown);
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::DeviceLost:
            countdown_.cancel();
            emit(C::CancelCountdown);
            emit(C::DeviceLostNotice);
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::TimerTick:
            if (countdown_.tick() == Countdown::Tick::Fired) {
                emit(C::StartRecording);
                st_ = DeckState::Recording;
            }
            return true;
        default: return false;
        }

    case DeckState::Recording:
    case DeckState::RecordingPaused: {
        const bool paused = st_ == DeckState::RecordingPaused;
        switch (e) {
        case DeckEvent::Stop:
        case DeckEvent::AppLeave:
            emit(C::StopRecording);
            has_take_ = true;
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::DeviceLost:
            emit(C::StopRecording);
            emit(C::DeviceLostNotice);
            has_take_ = true;
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::PlayPause:
            emit(paused ? C::ResumeRecording : C::PauseRecording);
            st_ = paused ? DeckState::Recording : DeckState::RecordingPaused;
            return true;
        default: return false;
        }
    }

    case DeckState::Playing:
    case DeckState::PlayingPaused: {
        const bool paused = st_ == DeckState::PlayingPaused;
        switch (e) {
        case DeckEvent::Rec:
            clear_hold();
            emit(C::StopPlayback);
            start_take_or_countdown();
            return true;
        case DeckEvent::Stop:
        case DeckEvent::AppLeave:
            clear_hold();
            emit(C::StopPlayback);
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::PlayPause:
            emit(paused ? C::ResumePlayback : C::PausePlayback);
            st_ = paused ? DeckState::Playing : DeckState::PlayingPaused;
            return true;
        case DeckEvent::RewDown: key_down(Direction::Reverse); return true;
        case DeckEvent::FwdDown: key_down(Direction::Forward); return true;
        case DeckEvent::RewUp: key_up(Direction::Reverse); return true;
        case DeckEvent::FwdUp: key_up(Direction::Forward); return true;
        case DeckEvent::TimerTick:
            if (hold_) {
                const uint64_t now = clock_.mono_ms();
                if (now - last_jump_ms_ >= kJumpRepeatMs) {
                    last_jump_ms_ = now;
                    emit(C::Jump, hold_dir_);
                }
            }
            return true;
        case DeckEvent::TakeEnded:
            clear_hold();
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::DeviceLost:
            if (paused) return true;
            emit(C::PausePlayback);
            emit(C::DeviceLostNotice);
            st_ = DeckState::PlayingPaused;
            return true;
        default: return false;
        }
    }

    case DeckState::ScanningForward:
    case DeckState::ScanningReverse: {
        const DeckEvent my_up = st_ == DeckState::ScanningForward ? DeckEvent::FwdUp : DeckEvent::RewUp;
        if (e == my_up) {
            emit(C::EndScan);
            st_ = resume_paused_ ? DeckState::PlayingPaused : DeckState::Playing;
            return true;
        }
        switch (e) {
        case DeckEvent::Rec:
            emit(C::StopPlayback);
            start_take_or_countdown();
            return true;
        case DeckEvent::Stop:
        case DeckEvent::AppLeave:
            emit(C::StopPlayback);
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::PlayPause:
            emit(C::EndScan);
            emit(resume_paused_ ? C::ResumePlayback : C::PausePlayback);
            st_ = resume_paused_ ? DeckState::Playing : DeckState::PlayingPaused;
            return true;
        case DeckEvent::TakeEnded:
            st_ = DeckState::Idle;
            return true;
        case DeckEvent::DeviceLost:
            emit(C::PausePlayback);
            emit(C::DeviceLostNotice);
            st_ = DeckState::PlayingPaused;
            return true;
        case DeckEvent::RewDown:
        case DeckEvent::FwdDown:
        case DeckEvent::RewUp:
        case DeckEvent::FwdUp:
        case DeckEvent::TimerTick:
            return true; /* the other key, or a repeat: ignored while scanning */
        default: return false;
        }
    }

    case DeckState::kCount: break;
    }
    return false;
}

bool Deck::key_enabled(DeckKey k) const {
    const bool rec_side = st_ == DeckState::Countdown || st_ == DeckState::Recording || st_ == DeckState::RecordingPaused;
    switch (k) {
    case DeckKey::Rew:
    case DeckKey::Fwd:
        if (rec_side) return false;
        return st_ == DeckState::Idle ? has_take_ : true;
    case DeckKey::PlayPause:
        if (st_ == DeckState::Countdown) return false;
        return st_ == DeckState::Idle ? has_take_ : true;
    case DeckKey::Stop: return st_ != DeckState::Idle;
    case DeckKey::Rec: return !rec_side;
    case DeckKey::kCount: break;
    }
    return false;
}

bool Deck::rec_latched() const {
    return st_ == DeckState::Countdown || st_ == DeckState::Recording || st_ == DeckState::RecordingPaused;
}

bool Deck::shows_pause_icon() const {
    return st_ == DeckState::Recording || st_ == DeckState::Playing || st_ == DeckState::ScanningForward ||
           st_ == DeckState::ScanningReverse;
}

bool Deck::key_active(DeckKey k) const {
    switch (k) {
    case DeckKey::Rec: return rec_latched();
    case DeckKey::PlayPause: return shows_pause_icon();
    case DeckKey::Rew: return st_ == DeckState::ScanningReverse || (hold_ && hold_dir_ == Direction::Reverse);
    case DeckKey::Fwd: return st_ == DeckState::ScanningForward || (hold_ && hold_dir_ == Direction::Forward);
    case DeckKey::Stop:
    case DeckKey::kCount: break;
    }
    return false;
}

} // namespace cassette
