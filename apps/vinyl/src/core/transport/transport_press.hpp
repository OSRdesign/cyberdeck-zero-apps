/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl transport press: what one press of "previous" or "next" does, the same for every path that has one (owner ruling
 * 2026-11-12): the Left / Right arrows on now playing, the comma / dot keys, the on-screen previous and next buttons
 * (now playing, the browser's mini player) and the media keys (KEY_PREVIOUSSONG / KEY_NEXTSONG).
 *
 *   Next, short press      the next track of the queue (Queue::next_after with user = true: at the end with repeat off it
 *                          stays on the last track; repeat all and repeat one wrap to the first)
 *   Previous, short press  more than kRestartAfterMs into the track: back to its start; within it: the previous track of
 *                          the queue (at the first track: back to its start, whatever the repeat mode)
 *   long press (held for   a scan at kScanSpeed times real time with the output muted: the position moves by periodic
 *   kHoldMs, then while    seeks; on release the output comes back and playback goes on from there (playing or paused as
 *   held)                  it was: the scan never plays or pauses). Forward, the end of the track goes on to the next one
 *                          and keeps scanning while held; at the end of the queue it stays near the end. Backward, it stops
 *                          at 0:00 of the track (never the previous track).
 * A press acts on its release, so a long press never also acts as a short one. A second press while one is held (the
 * auto-repeat of a key, or the other direction) is ignored. A touch that slides off its button (cancel) ends a scan but
 * does no short action. The caller gives the time of every call (a monotonic ms clock): the tests use a fake one. The
 * Shift+Left / Shift+Right 10 s seeks and the seek bar are not this module's. LVGL-free.
 */
#pragma once

#include "../queue/queue.hpp"

#include <cstdint>

namespace vinyl {

enum class TransportKey { Prev, Next };

/* What the module drives: the controller (vinyl_app.cpp) and the tests' fake. */
class TransportHost {
public:
    virtual ~TransportHost() = default;
    virtual bool has_track() const = 0;
    virtual uint64_t position_ms() const = 0; /* the heard position in the current track */
    virtual uint64_t duration_ms() const = 0; /* 0 = not known */
    virtual void seek(uint64_t ms) = 0;       /* in the current track; keeps playing or paused */
    virtual bool go_next() = 0;               /* false: there is none (transport_next_pos) */
    virtual bool go_prev() = 0;               /* false: there is none (transport_prev_pos) */
    virtual void mute(bool on) = 0;           /* the scan's silence */
};

/* The queue positions the host goes to (-1: none). */
int transport_next_pos(const Queue &q);
int transport_prev_pos(const Queue &q);

class TransportPress {
public:
    static constexpr uint64_t kRestartAfterMs = 2000; /* the owner's choice; common players use 3 s, Totem 4 s, Jellyfin 5 s */
    static constexpr uint64_t kHoldMs = 500;          /* a press held this long is a long press */
    static constexpr uint64_t kScanSpeed = 3;         /* times real time */
    static constexpr uint64_t kSeekEveryMs = 100;     /* the scan's seeks at most this often */
    static constexpr uint64_t kEndMarginMs = 1000;    /* a scan that cannot go on stays this far before the end */

    explicit TransportPress(TransportHost &host) : host_(host) {}

    void press(TransportKey k, uint64_t now_ms);
    void release(TransportKey k, uint64_t now_ms);
    void cancel(TransportKey k, uint64_t now_ms);
    /* Call it often (the controller's 100 ms tick): it turns a held press into a scan and moves the scan. */
    void tick(uint64_t now_ms);

    bool held() const { return held_; }
    bool scanning() const { return scanning_; }
    /* the position the scan has reached (the time and the seek bar show it while scanning) */
    uint64_t scan_position() const { return pos_; }

private:
    void step(uint64_t now_ms);
    void end(uint64_t now_ms);
    void reset();

    TransportHost &host_;
    bool held_ = false, scanning_ = false, long_ = false, stuck_ = false;
    TransportKey key_ = TransportKey::Next;
    uint64_t pressed_at_ = 0, pos_at_press_ = 0, last_ = 0, last_seek_ = 0, pos_ = 0;
};

}  // namespace vinyl
