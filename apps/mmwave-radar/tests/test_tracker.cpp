/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the tracker (trail, id, smoothed speed, history), driven by scripted frames and the walk fixture.
 */
#include "check.hpp"

#include "frame.hpp"
#include "tracker.hpp"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mmw;

namespace {

Frame make(int x0, int y0, int v0, int x1 = 0, int y1 = 0, int v1 = 0)
{
    Frame f;
    for (int i = 0; i < 3; ++i) f.target[i].slot = i + 1;
    auto set = [&](int i, int x, int y, int v) {
        Target &t = f.target[i];
        t.present = true;
        t.x_mm = x;
        t.y_mm = y;
        t.speed_cm_s = v;
        t.resolution_mm = 330;
        t.distance_mm = static_cast<int>(std::lround(std::hypot(x, y)));
        t.direction = v < 0 ? Direction::Approaching : v > 0 ? Direction::Receding : Direction::Still;
    };
    if (y0) set(0, x0, y0, v0);
    if (y1) set(1, x1, y1, v1);
    return f;
}

std::vector<Frame> walk_frames()
{
    const std::string bin = test_read_file(test_data_dir() + "ld2452-9600-walk-15s.bin");
    Decoder dec;
    std::vector<Frame> v;
    dec.feed(reinterpret_cast<const std::uint8_t *>(bin.data()), bin.size(), &v);
    return v;
}

void test_walk()
{
    const std::vector<Frame> frames = walk_frames();
    Tracker tr;
    std::int64_t t = 0;
    std::uint32_t id = 0;
    std::size_t max_trail = 0;
    for (const Frame &f : frames) {
        tr.update(f, t);
        const SlotState &s = tr.slot(0);
        if (s.active) {
            if (!id) id = s.id;
            CHECK_EQ(s.id, id); /* one person, one id for the whole walk */
        }
        max_trail = std::max(max_trail, s.trail.size());
        if (!s.trail.empty()) CHECK(t - s.trail.front().t_ms <= 2000);
        CHECK(!tr.slot(1).active && !tr.slot(2).active);
        t += 52;
    }
    CHECK(id != 0);
    CHECK(max_trail >= 36 && max_trail <= 41); /* about 2 s of frames at 52 ms */
    const SlotState &s = tr.slot(0);
    CHECK(s.active && s.present_now);
    CHECK_EQ(s.last.distance_mm, 953);
    CHECK_EQ(tr.frames(), 290u);
    CHECK(s.speed_smooth_cm_s < 0 && s.speed_smooth_cm_s > -20); /* still near -17 at the end */
    /* history: about 3 samples a second for at most 30 s; 15 s here */
    const auto &h = s.history;
    CHECK(h.size() >= 38 && h.size() <= 46); /* one per 7 frames of 52 ms */
    CHECK(h.back().t_ms - h.front().t_ms <= 30000);
    CHECK(!h.front().valid); /* the first 1.5 s: no target */
    CHECK(h.back().valid);
    CHECK(std::abs(h.back().distance_mm - 953) < 30); /* the last sample is a little older than the last frame */
    /* the first samples are 333 ms apart */
    CHECK(h[1].t_ms - h[0].t_ms >= 333 && h[1].t_ms - h[0].t_ms < 400);
}

void test_ids_and_drop()
{
    Tracker tr;
    tr.update(make(100, 1000, -17), 0);
    const std::uint32_t a = tr.slot(0).id;
    CHECK(a != 0 && tr.slot(0).active);
    /* one empty frame is a glitch: same id */
    tr.update(Frame(), 52);
    CHECK(tr.slot(0).active);
    CHECK(!tr.slot(0).present_now);
    tr.update(make(110, 990, -17), 104);
    CHECK_EQ(tr.slot(0).id, a);
    CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(2));
    /* gone for 500 ms: released, trail cleared */
    for (std::int64_t t = 156; t <= 700; t += 52) tr.update(Frame(), t);
    CHECK(!tr.slot(0).active);
    CHECK(tr.slot(0).trail.empty());
    /* the next target is a new id with a fresh trail and speed */
    tr.update(make(0, 2000, 34), 800);
    CHECK(tr.slot(0).id > a);
    CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(1));
    CHECK_EQ(tr.slot(0).speed_smooth_cm_s, 34.0);
    /* two people: independent ids per slot */
    Tracker t2;
    t2.update(make(0, 1000, -17, 500, 3000, 34), 0);
    CHECK(t2.slot(0).id != t2.slot(1).id);
    CHECK(t2.slot(0).active && t2.slot(1).active && !t2.slot(2).active);
    t2.update(make(0, 1000, -17), 52);
    t2.update(make(0, 1000, -17), 600);
    CHECK(t2.slot(0).active && !t2.slot(1).active);
    /* release by time alone (the frames stopped): tick() */
    Tracker t3;
    t3.update(make(0, 1000, -17), 0);
    t3.tick(300);
    CHECK(t3.slot(0).active && t3.slot(0).present_now);
    t3.tick(600);
    CHECK(!t3.slot(0).active && !t3.slot(0).present_now);
    /* clear() empties it but ids keep counting */
    Tracker t4;
    t4.update(make(0, 1000, 0), 0);
    const std::uint32_t before = t4.slot(0).id;
    t4.clear();
    CHECK(!t4.slot(0).active && t4.frames() == 0 && t4.slot(0).history.empty());
    t4.update(make(0, 1000, 0), 10);
    CHECK(t4.slot(0).id > before);
}

void test_trail_and_smoothing()
{
    Tracker tr;
    for (int i = 0; i < 100; ++i) tr.update(make(i, 1000 + i, -50), i * 50);
    const SlotState &s = tr.slot(0);
    CHECK_EQ(s.trail.size(), static_cast<std::size_t>(41)); /* 2000 ms / 50 ms, both ends inclusive */
    CHECK(s.trail.back().t_ms - s.trail.front().t_ms <= 2000);
    CHECK_EQ(s.trail.back().x_mm, 99);
    CHECK_EQ(s.trail.back().y_mm, 1099);
    CHECK_EQ(s.speed_smooth_cm_s, -50.0);
    /* a step in speed is smoothed, converging without overshoot */
    Tracker t2;
    t2.update(make(0, 1000, 0), 0);
    double prev = 0;
    for (int i = 1; i < 30; ++i) {
        t2.update(make(0, 1000, 100), i * 50);
        const double v = t2.slot(0).speed_smooth_cm_s;
        CHECK(v > prev && v < 100.0);
        prev = v;
    }
    CHECK(prev > 99.0 || prev > 95.0);
    /* the trail empties by time alone */
    t2.tick(2000 + 29 * 50 + 100);
    CHECK(t2.slot(0).trail.empty());
}

void test_history()
{
    Tracker tr;
    /* 40 s of frames at 50 ms: history never holds more than 30 s */
    for (int i = 0; i < 800; ++i) tr.update(make(0, 1000 + i, 0), i * 50);
    const auto &h = tr.slot(0).history;
    CHECK(h.back().t_ms - h.front().t_ms <= 30000);
    CHECK(h.back().t_ms - h.front().t_ms >= 29500);
    CHECK(h.size() >= 84 && h.size() <= 91); /* about 3 per second (every 350 ms on a 50 ms frame clock) */
    for (std::size_t i = 1; i < h.size(); ++i) CHECK(h[i].t_ms - h[i - 1].t_ms >= 333);
    CHECK(h.back().valid);
    /* empty slots get invalid samples (a gap in the graph) */
    CHECK(!tr.slot(1).history.empty());
    CHECK(!tr.slot(1).history.back().valid);
    /* no frames yet: no history */
    Tracker idle;
    idle.tick(1000);
    CHECK(idle.slot(0).history.empty());
    /* determinism: the same input gives the same output */
    Tracker a, b;
    for (int i = 0; i < 300; ++i) {
        a.update(make(i % 50, 1000 + i, (i % 7) * 17 - 50), i * 53);
        b.update(make(i % 50, 1000 + i, (i % 7) * 17 - 50), i * 53);
    }
    CHECK_EQ(a.slot(0).history.size(), b.slot(0).history.size());
    CHECK_EQ(a.slot(0).speed_smooth_cm_s, b.slot(0).speed_smooth_cm_s);
    CHECK_EQ(a.slot(0).trail.size(), b.slot(0).trail.size());
}

} // namespace

void test_tracker()
{
    test_walk();
    test_ids_and_drop();
    test_trail_and_smoothing();
    test_history();
}
