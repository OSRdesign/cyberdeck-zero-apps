/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: the tracker (trail, id, smoothed speed, history), driven by scripted frames and the walk fixture.
 */
#include "check.hpp"

#include "ld2450.hpp"
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

/* Held (stale) targets: byte-identical x, y, speed, resolution for kHeldMs of continuous frames. */
void test_held()
{
    CHECK_EQ(static_cast<int>(kHeldMs), 3000);
    /* exactly at 3000 ms, not at 2999; frames every 100 ms with identical values */
    {
        Tracker tr;
        for (int t = 0; t <= 2900; t += 100) {
            tr.update(make(500, 2000, 0), t);
            CHECK(!tr.slot(0).held);
        }
        tr.update(make(500, 2000, 0), 2999);
        CHECK(!tr.slot(0).held);
        CHECK_EQ(tr.held_count(), 0);
        tr.update(make(500, 2000, 0), 3000);
        CHECK(tr.slot(0).held);
        CHECK_EQ(tr.held_count(), 1);
        tr.update(make(500, 2000, 0), 3100);
        CHECK(tr.slot(0).held); /* stays held */
        CHECK(tr.slot(0).active && tr.slot(0).present_now);
    }
    /* any single changed value resets (x, y, speed, resolution) and the count restarts from that frame */
    for (int what = 0; what < 4; ++what) {
        Tracker tr;
        for (int t = 0; t <= 3500; t += 100) tr.update(make(500, 2000, 0), t);
        CHECK(tr.slot(0).held);
        Frame f = make(500, 2000, 0);
        Target &g = f.target[0];
        if (what == 0) g.x_mm += 1;
        if (what == 1) g.y_mm += 1;
        if (what == 2) g.speed_cm_s += 1;
        if (what == 3) g.resolution_mm += 1;
        tr.update(f, 3600);
        CHECK(!tr.slot(0).held);
        const std::uint32_t id = tr.slot(0).id;
        for (int t = 3700; t < 6600; t += 100) {
            tr.update(f, t);
            CHECK(!tr.slot(0).held); /* under 3000 ms since the change */
        }
        tr.update(f, 6599);
        CHECK(!tr.slot(0).held);
        tr.update(f, 6600);
        CHECK(tr.slot(0).held);
        CHECK_EQ(tr.slot(0).id, id); /* held detection does not change identity */
    }
    /* the target leaves the frame (even for one frame): reset, and it has to be 3 s again after it comes back */
    {
        Tracker tr;
        for (int t = 0; t <= 3500; t += 100) tr.update(make(500, 2000, 0), t);
        CHECK(tr.slot(0).held);
        const std::uint32_t id = tr.slot(0).id;
        tr.update(make(0, 0, 0), 3600); /* empty frame */
        CHECK(!tr.slot(0).held);
        CHECK(tr.slot(0).active);
        CHECK_EQ(tr.held_count(), 0);
        for (int t = 3700; t < 6700; t += 100) {
            tr.update(make(500, 2000, 0), t);
            CHECK(!tr.slot(0).held);
        }
        tr.update(make(500, 2000, 0), 6700);
        CHECK(tr.slot(0).held); /* 3000 ms after 3700 */
        CHECK_EQ(tr.slot(0).id, id);
    }
    /* released after drop_ms, then back: a new target, not held at once */
    {
        Tracker tr;
        for (int t = 0; t <= 3500; t += 100) tr.update(make(500, 2000, 0), t);
        tr.tick(4200); /* nothing for 700 ms: released */
        CHECK(!tr.slot(0).active);
        CHECK(!tr.slot(0).held);
        tr.update(make(500, 2000, 0), 4300);
        CHECK(!tr.slot(0).held);
        CHECK(tr.slot(0).active);
    }
    /* the frames stop: a held target does not stay held */
    {
        Tracker tr;
        for (int t = 0; t <= 3500; t += 100) tr.update(make(500, 2000, 0), t);
        tr.tick(3700);
        CHECK(tr.slot(0).held); /* a glitch shorter than drop_ms */
        tr.tick(4100);
        CHECK(!tr.slot(0).held);
        CHECK_EQ(tr.held_count(), 0);
    }
    /* two targets are independent: T2 is stale from the start, T1 moves, then T1 freezes too */
    {
        Tracker tr;
        for (int t = 0; t <= 4000; t += 100) tr.update(make(100 + t / 10, 1500, -17, -900, 3000, 0), t);
        CHECK(!tr.slot(0).held);
        CHECK(tr.slot(1).held);
        CHECK_EQ(tr.held_count(), 1);
        for (int t = 4100; t < 7100; t += 100) tr.update(make(500, 1500, 0, -900, 3000, 0), t);
        CHECK(!tr.slot(0).held && tr.slot(1).held); /* frozen since 4100: held from 7100 */
        tr.update(make(500, 1500, 0, -900, 3000, 0), 7100);
        CHECK(tr.slot(0).held && tr.slot(1).held);
        CHECK_EQ(tr.held_count(), 2);
        /* T2 changes: only T2 resets */
        tr.update(make(500, 1500, 0, -899, 3000, 0), 7200);
        CHECK(tr.slot(0).held && !tr.slot(1).held);
        CHECK_EQ(tr.held_count(), 1);
    }
    /* a real still person: 1 mm of jitter at least every 2 s keeps the target live for over a minute */
    {
        Tracker tr;
        for (int t = 0; t <= 70000; t += 100) {
            const int jitter = (t / 2000) % 2; /* 0, 1, 0, 1 mm: a change every 2 s */
            tr.update(make(500 + jitter, 2000, 0), t);
            CHECK(!tr.slot(0).held);
        }
        CHECK_EQ(tr.held_count(), 0);
    }
    /* clear() */
    {
        Tracker tr;
        for (int t = 0; t <= 3500; t += 100) tr.update(make(500, 2000, 0), t);
        tr.clear();
        CHECK(!tr.slot(0).held);
        CHECK_EQ(tr.held_count(), 0);
    }
}

/* The jump rule (tracker.hpp): a measurement farther from the slot's last one than kMaxJumpSpeed for the elapsed time plus
 * kJumpSlack is a new target. */
void test_jump()
{
    /* run 7 of the Bluetooth board test: 2 m / +30 deg, 0.8 km/h, then 0.3 s later a raw frame of 41 m / -47 deg, 30 km/h */
    {
        Tracker tr;
        for (std::int64_t t = 0; t <= 3500; t += 100) tr.update(make(1000, 1732, 8), t); /* held for over 3 s */
        CHECK(tr.slot(0).held);
        CHECK(tr.slot(0).trail.size() > 5);
        const std::uint32_t a = tr.slot(0).id;
        tr.update(make(-29986, 27962, 833), 3800);
        const SlotState &s = tr.slot(0);
        CHECK(s.active && s.present_now);
        CHECK(s.id != a && s.id > a);
        CHECK(!s.held);
        CHECK_EQ(s.trail.size(), static_cast<std::size_t>(1));
        CHECK_EQ(s.same_since_ms, static_cast<std::int64_t>(3800));
        CHECK_EQ(s.first_seen_ms, static_cast<std::int64_t>(3800));
        CHECK_EQ(s.speed_smooth_cm_s, 833.0); /* no smoothing from the old target */
        CHECK_EQ(s.last.x_mm, -29986);
        CHECK(!tr.slot(1).active && !tr.slot(2).active); /* the old target is gone, not kept beside the new one */
        CHECK_EQ(tr.held_count(), 0);
        /* and it holds only as long as the new one is there: the old place again is another jump */
        const std::uint32_t b = s.id;
        tr.update(make(1000, 1732, 8), 3900);
        CHECK(tr.slot(0).id > b);
    }
    /* 3 m in 0.15 s (20 m/s) is a person or a car: the same id, the trail goes on */
    {
        Tracker tr;
        tr.update(make(0, 2000, -17), 0);
        const std::uint32_t a = tr.slot(0).id;
        tr.update(make(0, 5000, -17), 150);
        CHECK_EQ(tr.slot(0).id, a);
        CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(2));
    }
    /* exactly at the limit stays, one mm over is new: 100 ms allow 40 m/s * 0.1 s + 1 m = 5 m (3-4-5 in whole mm) */
    {
        Tracker tr;
        tr.update(make(0, 2000, 0), 0);
        const std::uint32_t a = tr.slot(0).id;
        tr.update(make(3000, 6000, 0), 100);
        CHECK_EQ(tr.slot(0).id, a);
        tr.update(make(3000 + 3000, 6000 + 4000, 0), 200);
        CHECK_EQ(tr.slot(0).id, a);
        Tracker over;
        over.update(make(0, 2000, 0), 0);
        over.update(make(3001, 6000, 0), 100); /* 5000.2 mm */
        CHECK(over.slot(0).id > a);
        Tracker straight;
        straight.update(make(0, 2000, 0), 0);
        straight.update(make(0, 7000, 0), 100); /* 5000 mm: at the limit */
        CHECK_EQ(straight.slot(0).id, 1u);
        Tracker straight2;
        straight2.update(make(0, 2000, 0), 0);
        straight2.update(make(0, 7001, 0), 100);
        CHECK(straight2.slot(0).id > 1u);
    }
    /* the first measurement has nothing to jump from, however far */
    {
        Tracker tr;
        tr.update(make(-29986, 27962, 833), 0);
        CHECK(tr.slot(0).active);
        CHECK_EQ(tr.slot(0).id, 1u);
        CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(1));
    }
    /* a gap longer than drop_ms: the slot was released first (the Link ticks), the next one is a new target anyway */
    {
        Tracker tr;
        tr.update(make(0, 2000, 0), 0);
        const std::uint32_t a = tr.slot(0).id;
        tr.tick(600);
        CHECK(!tr.slot(0).active);
        tr.update(make(100, 2100, 0), 700); /* near the old place: still a new id, the slot was released */
        CHECK(tr.slot(0).id > a);
        tr.tick(700);
        tr.tick(1500);
        tr.update(make(-29986, 27962, 833), 4000);
        CHECK(tr.slot(0).id > a + 1);
        CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(1));
        /* with the LD2451's 3 s: a 2.8 s gap keeps the slot, and 2.8 s allow 112 m + 1 m: no jump */
        TrackerConfig c;
        c.drop_ms = 3000;
        Tracker t451(c);
        t451.update(make(0, 2000, 0), 0);
        t451.tick(2800);
        t451.update(make(-29986, 27962, 833), 2800);
        CHECK_EQ(t451.slot(0).id, 1u);
    }
    /* frames with the same time stamp (read in one poll): no elapsed time, the rule does not judge them */
    {
        Tracker tr;
        tr.update(make(0, 2000, 0), 500);
        tr.update(make(0, 7000, 0), 500);
        CHECK_EQ(tr.slot(0).id, 1u);
    }
    /* two targets (the LD2450 family): only the slot that jumped gets a new id, the other walks on */
    {
        Tracker tr;
        tr.update(make(-500, 1500, -10, 900, 3000, 10), 0);
        const std::uint32_t a = tr.slot(0).id, b = tr.slot(1).id;
        CHECK(a != b);
        for (int i = 1; i <= 10; ++i) tr.update(make(-500 + 20 * i, 1500 + 15 * i, -10, 900 - 10 * i, 3000 + 10 * i, 10), 52 * i);
        CHECK_EQ(tr.slot(0).id, a);
        CHECK_EQ(tr.slot(1).id, b);
        tr.update(make(-300, 1700, -10, 8000, 9000, 10), 52 * 11); /* the second jumps 9 m in 52 ms */
        CHECK_EQ(tr.slot(0).id, a);
        CHECK(tr.slot(1).id > b);
        CHECK_EQ(tr.slot(0).trail.size(), static_cast<std::size_t>(12));
        CHECK_EQ(tr.slot(1).trail.size(), static_cast<std::size_t>(1));
    }
}

} // namespace

void test_tracker()
{
    test_jump();
    test_held();
    test_walk();
    test_ids_and_drop();
    test_trail_and_smoothing();
    test_history();
}
