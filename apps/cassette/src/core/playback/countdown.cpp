/* SPDX-License-Identifier: MIT */
#include "countdown.hpp"

namespace cassette {

bool Countdown::start(unsigned seconds) {
    if (seconds == 0) {
        active_ = false;
        return false;
    }
    active_ = true;
    start_ms_ = clock_.mono_ms();
    total_ms_ = seconds * 1000u;
    return true;
}

void Countdown::cancel() { active_ = false; }

uint64_t Countdown::left_ms() const {
    if (!active_) return 0;
    const uint64_t now = clock_.mono_ms();
    const uint64_t el = now > start_ms_ ? now - start_ms_ : 0;
    return el >= total_ms_ ? 0 : total_ms_ - el;
}

Countdown::Remaining Countdown::remaining() const {
    Remaining r;
    if (!active_ || total_ms_ == 0) return r;
    const uint64_t left = left_ms();
    r.seconds = static_cast<unsigned>((left + 999) / 1000);
    r.fraction = static_cast<float>(left) / static_cast<float>(total_ms_);
    return r;
}

Countdown::Tick Countdown::tick() {
    if (!active_) return Tick::Idle;
    if (left_ms() > 0) return Tick::Running;
    active_ = false;
    return Tick::Fired;
}

} // namespace cassette
