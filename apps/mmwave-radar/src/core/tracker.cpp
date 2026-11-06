/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the tracker (see tracker.hpp).
 */
#include "tracker.hpp"

namespace mmw {

void Tracker::clear()
{
    for (SlotState &s : slots_) s = SlotState();
    frames_ = 0;
    sampled_ = false;
    /* ids keep counting: an id is never reused in a session */
}

void Tracker::update(const Frame &f, std::int64_t t_ms)
{
    ++frames_;
    last_frame_ms_ = t_ms;
    for (int i = 0; i < 3; ++i) {
        SlotState &s = slots_[i];
        const Target &t = f.target[i];
        const bool was_present = s.present_now;
        s.present_now = t.present;
        if (!t.present) {
            s.held = false; /* gone from the frame: the count restarts when it comes back */
            continue;
        }
        const bool same = s.active && was_present && s.last.x_mm == t.x_mm && s.last.y_mm == t.y_mm &&
                          s.last.speed_cm_s == t.speed_cm_s && s.last.resolution_mm == t.resolution_mm;
        if (!same) {
            s.same_since_ms = t_ms;
            s.held = false;
        } else if (t_ms - s.same_since_ms >= kHeldMs) {
            s.held = true;
        }
        if (!s.active) {
            s.active = true;
            s.id = next_id_++;
            s.first_seen_ms = t_ms;
            s.trail.clear();
            s.speed_smooth_cm_s = t.speed_cm_s;
        } else {
            s.speed_smooth_cm_s += cfg_.speed_alpha * (t.speed_cm_s - s.speed_smooth_cm_s);
        }
        s.last = t;
        s.last_seen_ms = t_ms;
        TrailPoint p;
        p.t_ms = t_ms;
        p.x_mm = t.x_mm;
        p.y_mm = t.y_mm;
        s.trail.push_back(p);
    }
    expire(t_ms);
    sample(t_ms);
}

int Tracker::held_count() const
{
    int n = 0;
    for (const SlotState &s : slots_)
        if (s.active && s.present_now && s.held) ++n;
    return n;
}

void Tracker::tick(std::int64_t t_ms)
{
    if (frames_ > 0 && t_ms - last_frame_ms_ >= cfg_.drop_ms)
        for (SlotState &s : slots_) {
            s.present_now = false; /* the frames stopped */
            s.held = false;
        }
    expire(t_ms);
    sample(t_ms);
}

void Tracker::expire(std::int64_t t_ms)
{
    for (SlotState &s : slots_) {
        if (s.active && t_ms - s.last_seen_ms >= cfg_.drop_ms) {
            s.present_now = false;
            s.active = false;
            s.held = false;
            s.trail.clear();
        }
        /* a slot seen in an older frame is not "now" any more once time moved on past drop */
        while (!s.trail.empty() && t_ms - s.trail.front().t_ms > cfg_.trail_ms) s.trail.pop_front();
    }
}

void Tracker::sample(std::int64_t t_ms)
{
    if (sampled_ && t_ms - last_sample_ms_ < cfg_.history_step_ms) return;
    if (!sampled_ && frames_ == 0) return; /* nothing seen yet: no history */
    sampled_ = true;
    last_sample_ms_ = t_ms;
    for (SlotState &s : slots_) {
        HistorySample h;
        h.t_ms = t_ms;
        h.valid = s.active && s.present_now;
        if (h.valid) {
            h.distance_mm = s.last.distance_mm;
            h.speed_cm_s = s.speed_smooth_cm_s;
        }
        s.history.push_back(h);
        while (!s.history.empty() && t_ms - s.history.front().t_ms > cfg_.history_ms) s.history.pop_front();
    }
}

} // namespace mmw
