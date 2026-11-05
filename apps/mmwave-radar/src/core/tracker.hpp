/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: per-slot state from the decoded frames: a short trail, a smoothed speed, a stable target id and a
 * rolling history of distance and speed for the graph. Pure and deterministic: every call takes the time in ms (any
 * monotonic origin); there is no clock inside.
 *
 * The radar's three slots are taken as the target identities (the slot number stays with a person as long as the radar
 * keeps it; whether it does with two people is not verified, report 056 section 8). A slot that disappears for less
 * than drop_ms keeps its id (one empty frame is a glitch, not a new person); after drop_ms the slot is released and
 * the next target in it gets a new id. A new id restarts the trail and the smoothed speed.
 */
#pragma once

#include "frame.hpp"

#include <cstdint>
#include <deque>

namespace mmw {

struct TrackerConfig {
    std::int64_t trail_ms = 2000;          /* trail length */
    std::int64_t history_ms = 30000;       /* graph history length */
    std::int64_t history_step_ms = 333;    /* one history sample every this (3 per second) */
    std::int64_t drop_ms = 500;            /* a slot unseen for this long is released */
    double speed_alpha = 0.3;              /* smoothing: s += alpha * (v - s) per frame */
};

struct TrailPoint {
    std::int64_t t_ms = 0;
    int x_mm = 0, y_mm = 0;
};

struct HistorySample {
    std::int64_t t_ms = 0;
    bool valid = false;         /* a target was there */
    int distance_mm = 0;
    double speed_cm_s = 0.0;    /* smoothed */
};

struct SlotState {
    bool active = false;            /* has a target (seen within drop_ms) */
    bool present_now = false;       /* in the latest frame */
    std::uint32_t id = 0;           /* stable while the slot stays active; 0 = never had one */
    Target last;                    /* the latest values (kept while active) */
    double speed_smooth_cm_s = 0.0;
    std::int64_t first_seen_ms = 0, last_seen_ms = 0;
    std::deque<TrailPoint> trail;   /* oldest first, within trail_ms of the newest point */
    std::deque<HistorySample> history;
};

class Tracker {
public:
    explicit Tracker(const TrackerConfig &cfg = TrackerConfig()) : cfg_(cfg) {}

    /* One decoded frame, arrived at t_ms. */
    void update(const Frame &f, std::int64_t t_ms);
    /* No frame, only time passing (releases stale slots, keeps the history sampling): call from a timer. */
    void tick(std::int64_t t_ms);
    void clear();

    const SlotState &slot(int index) const { return slots_[index]; } /* 0..2 */
    std::uint32_t frames() const { return frames_; }

private:
    void expire(std::int64_t t_ms);
    void sample(std::int64_t t_ms);
    TrackerConfig cfg_;
    SlotState slots_[3];
    std::uint32_t next_id_ = 1;
    std::uint32_t frames_ = 0;
    std::int64_t last_sample_ms_ = 0;
    std::int64_t last_frame_ms_ = 0;
    bool sampled_ = false;
};

} // namespace mmw
