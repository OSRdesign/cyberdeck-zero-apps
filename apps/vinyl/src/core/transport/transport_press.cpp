/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl transport press (see transport_press.hpp).
 */
#include "transport_press.hpp"

namespace vinyl {

int transport_next_pos(const Queue &q) {
    if (q.empty()) return -1;
    return q.next_after(q.current(), true);
}

int transport_prev_pos(const Queue &q) {
    if (q.empty() || q.current() <= 0) return -1; /* the first track restarts: no wrap to the last */
    return q.current() - 1;
}

void TransportPress::press(TransportKey k, uint64_t now_ms) {
    if (held_) return; /* the auto-repeat of the held key, or the other one while it is held */
    held_ = true;
    key_ = k;
    pressed_at_ = now_ms;
    pos_at_press_ = host_.has_track() ? host_.position_ms() : 0;
    long_ = scanning_ = stuck_ = false;
}

void TransportPress::tick(uint64_t now_ms) {
    if (!held_) return;
    if (!long_) {
        if (now_ms - pressed_at_ < kHoldMs) return;
        long_ = true; /* from here the release does no short action */
        if (!host_.has_track()) return;
        scanning_ = true;
        pos_ = host_.position_ms();
        last_ = now_ms;
        last_seek_ = 0;
        host_.mute(true);
        return;
    }
    if (scanning_) step(now_ms);
}

/* moves the scan to now_ms and seeks there (at most every kSeekEveryMs) */
void TransportPress::step(uint64_t now_ms) {
    const uint64_t dt = now_ms > last_ ? now_ms - last_ : 0;
    last_ = now_ms;
    const uint64_t d = dt * kScanSpeed;
    bool seek = false;
    if (key_ == TransportKey::Next) {
        if (stuck_) return;
        pos_ += d;
        const uint64_t dur = host_.duration_ms();
        if (dur && pos_ >= dur) {
            if (host_.go_next()) { /* the next track starts at 0:00; the scan goes on there */
                pos_ = 0;
                last_seek_ = now_ms;
                return;
            }
            pos_ = dur > kEndMarginMs ? dur - kEndMarginMs : 0; /* the end of the queue: stay near the end */
            stuck_ = true;
            seek = true;
        }
    } else {
        pos_ = pos_ > d ? pos_ - d : 0; /* stops at 0:00 of this track */
    }
    if (seek || last_seek_ == 0 || now_ms - last_seek_ >= kSeekEveryMs) {
        host_.seek(pos_);
        last_seek_ = now_ms ? now_ms : 1;
    }
}

void TransportPress::end(uint64_t now_ms) {
    step(now_ms);
    host_.seek(pos_);
    host_.mute(false);
}

void TransportPress::release(TransportKey k, uint64_t now_ms) {
    if (!held_ || k != key_) return;
    if (scanning_) {
        end(now_ms);
    } else if (!long_) {
        if (k == TransportKey::Next) {
            host_.go_next();
        } else if (pos_at_press_ > kRestartAfterMs || !host_.go_prev()) {
            host_.seek(0); /* restart the track (the first track always restarts) */
        }
    }
    reset();
}

void TransportPress::cancel(TransportKey k, uint64_t now_ms) {
    if (!held_ || k != key_) return;
    if (scanning_) end(now_ms);
    reset();
}

void TransportPress::reset() {
    held_ = scanning_ = long_ = stuck_ = false;
    pressed_at_ = pos_at_press_ = last_ = last_seek_ = 0;
}

}  // namespace vinyl
