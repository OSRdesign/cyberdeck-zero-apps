/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the transport press (core/transport, owner ruling 2026-11-12) with a fake host and a fake clock (every call
 * takes its time): short presses, the 2 s boundary, long presses that scan muted at 3x, the end of a track while
 * scanning, the paused state, the repeat modes, no double action.
 */
#include "check.hpp"

#include "seek/seek_scrub.hpp"
#include "transport/transport_press.hpp"

#include <vector>

using namespace vinyl;

namespace {

struct FakeHost final : TransportHost {
    Queue q;
    bool track = true, muted = false, playing = true;
    uint64_t pos = 0, dur = 240000;
    int nexts = 0, prevs = 0, mutes = 0, unmutes = 0;
    std::vector<uint64_t> seeks;

    FakeHost() { q.set({"a", "b", "c", "d"}, 1, "src"); }
    bool has_track() const override { return track; }
    uint64_t position_ms() const override { return pos; }
    uint64_t duration_ms() const override { return dur; }
    void seek(uint64_t ms) override {
        pos = ms;
        seeks.push_back(ms);
    }
    bool go_next() override {
        const int n = transport_next_pos(q);
        if (n < 0) return false;
        q.set_current(n);
        pos = 0;
        ++nexts;
        return true;
    }
    bool go_prev() override {
        const int n = transport_prev_pos(q);
        if (n < 0) return false;
        q.set_current(n);
        pos = 0;
        ++prevs;
        return true;
    }
    void mute(bool on) override {
        muted = on;
        ++(on ? mutes : unmutes);
    }
};

/* a press of `ms` with the controller's 100 ms ticks in between */
void hold(TransportPress &t, TransportKey k, uint64_t &now, uint64_t ms) {
    t.press(k, now);
    const uint64_t end = now + ms;
    while (now + 100 <= end) {
        now += 100;
        t.tick(now);
    }
    now = end;
    t.release(k, now);
}

void test_short_presses() {
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 1000;
    /* next: on the release, once */
    h.pos = 50000;
    t.press(TransportKey::Next, now);
    CHECK_EQ(h.nexts, 0);
    t.tick(now + 100);
    t.release(TransportKey::Next, now + 150);
    CHECK_EQ(h.nexts, 1);
    CHECK_EQ(h.q.current(), 2);
    CHECK(!h.muted && h.mutes == 0);
    /* previous after more than 2 s: the same track from its start */
    now = 5000;
    h.pos = 2001;
    hold(t, TransportKey::Prev, now, 120);
    CHECK_EQ(h.q.current(), 2);
    CHECK_EQ(h.prevs, 0);
    CHECK(!h.seeks.empty() && h.seeks.back() == 0);
    /* exactly 2.0 s is not more than 2 s: the previous track */
    h.pos = 2000;
    hold(t, TransportKey::Prev, now, 120);
    CHECK_EQ(h.prevs, 1);
    CHECK_EQ(h.q.current(), 1);
    /* within the first 2 s again: the previous track, then the first track restarts */
    h.pos = 400;
    hold(t, TransportKey::Prev, now, 120);
    CHECK_EQ(h.q.current(), 0);
    const size_t seeks_before = h.seeks.size();
    h.pos = 300;
    hold(t, TransportKey::Prev, now, 120);
    CHECK_EQ(h.q.current(), 0);
    CHECK_EQ(h.prevs, 2);
    CHECK_EQ(h.seeks.size(), seeks_before + 1);
    CHECK_EQ(h.seeks.back(), static_cast<uint64_t>(0));
    /* the position counts at the press, not at the release */
    h.q.set_current(2);
    h.pos = 1900;
    t.press(TransportKey::Prev, now);
    h.pos = 2300; /* it played on meanwhile */
    t.release(TransportKey::Prev, now + 400);
    CHECK_EQ(h.q.current(), 1);
    CHECK_EQ(h.prevs, 3);
}

void test_no_double_action() {
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 0;
    /* the auto-repeat of a held key and the other key while one is held do nothing */
    t.press(TransportKey::Next, now);
    t.press(TransportKey::Next, now + 30);
    t.press(TransportKey::Prev, now + 60);
    t.release(TransportKey::Prev, now + 90); /* not the held key: ignored */
    CHECK(t.held());
    t.release(TransportKey::Next, now + 120);
    CHECK_EQ(h.nexts, 1);
    CHECK_EQ(h.prevs, 0);
    /* a release without a press does nothing */
    t.release(TransportKey::Next, now + 200);
    CHECK_EQ(h.nexts, 1);
    /* a long press does no short action on release */
    now = 1000;
    h.pos = 10000;
    hold(t, TransportKey::Next, now, 900);
    CHECK_EQ(h.nexts, 1);
    CHECK_EQ(h.q.current(), 2);
    /* a cancelled touch does nothing */
    t.press(TransportKey::Next, now);
    t.cancel(TransportKey::Next, now + 100);
    CHECK_EQ(h.nexts, 1);
    CHECK(!t.held());
}

void test_scan_forward() {
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 10000;
    h.pos = 60000;
    t.press(TransportKey::Next, now);
    t.tick(now + 400);
    CHECK(!t.scanning() && h.mutes == 0); /* not yet a long press */
    t.tick(now + 500);
    CHECK(t.scanning());
    CHECK(h.muted);
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(60000));
    t.tick(now + 600); /* 100 ms at 3x */
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(60300));
    CHECK_EQ(h.seeks.back(), static_cast<uint64_t>(60300));
    t.tick(now + 700);
    t.tick(now + 800);
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(60900));
    t.release(TransportKey::Next, now + 850);
    CHECK(!t.scanning());
    CHECK(!h.muted);
    CHECK_EQ(h.unmutes, 1);
    CHECK_EQ(h.seeks.back(), static_cast<uint64_t>(61050)); /* the final position, 350 ms of scan = 1.05 s */
    CHECK_EQ(h.nexts, 0);
    CHECK(h.playing); /* the scan neither plays nor pauses */
}

void test_scan_backward() {
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 0;
    h.pos = 1000;
    t.press(TransportKey::Prev, now);
    for (uint64_t ms = 100; ms <= 1500; ms += 100) t.tick(now + ms);
    CHECK(t.scanning());
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(0)); /* stops at 0:00 */
    CHECK_EQ(h.prevs, 0);                                    /* never the previous track */
    CHECK_EQ(h.q.current(), 1);
    t.release(TransportKey::Prev, now + 1600);
    CHECK_EQ(h.seeks.back(), static_cast<uint64_t>(0));
    CHECK(!h.muted);
    CHECK_EQ(h.prevs, 0);
}

void test_scan_end_of_track() {
    /* the end of the track goes on to the next one and keeps scanning while held */
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 0;
    h.dur = 10000;
    h.pos = 9500;
    t.press(TransportKey::Next, now);
    t.tick(now + 500); /* the scan starts at 9.5 s */
    t.tick(now + 600); /* 9.8 s */
    t.tick(now + 700); /* 10.1 s: past the end */
    CHECK_EQ(h.nexts, 1);
    CHECK_EQ(h.q.current(), 2);
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(0));
    CHECK(t.scanning() && h.muted);
    t.tick(now + 800);
    t.tick(now + 900);
    CHECK_EQ(t.scan_position(), static_cast<uint64_t>(600));
    t.release(TransportKey::Next, now + 900);
    CHECK_EQ(h.nexts, 1); /* no short action on top */
    CHECK(!h.muted);
    CHECK_EQ(h.pos, static_cast<uint64_t>(600));

    /* at the end of the queue with repeat off: it stays near the end of the last track, once */
    FakeHost e;
    e.q.set_current(3);
    e.dur = 10000;
    e.pos = 9700;
    TransportPress te(e);
    now = 0;
    te.press(TransportKey::Next, now);
    for (uint64_t ms = 100; ms <= 1500; ms += 100) te.tick(now + ms);
    CHECK_EQ(e.nexts, 0);
    CHECK_EQ(e.q.current(), 3);
    CHECK_EQ(te.scan_position(), static_cast<uint64_t>(9000));
    te.release(TransportKey::Next, now + 1600);
    CHECK_EQ(e.pos, static_cast<uint64_t>(9000));
    CHECK(!e.muted);
}

void test_paused_and_no_track() {
    /* a paused track stays paused: the module never plays or pauses (the host has no such call) and the scan moves the
     * position by seeks only */
    FakeHost h;
    h.playing = false;
    TransportPress t(h);
    uint64_t now = 0;
    h.pos = 30000;
    hold(t, TransportKey::Next, now, 1000);
    CHECK(!h.playing);
    CHECK_EQ(h.pos, static_cast<uint64_t>(31500)); /* 500 ms of scan at 3x */
    /* nothing loaded: a long press is no scan and no short action */
    FakeHost n;
    n.track = false;
    TransportPress tn(n);
    now = 0;
    hold(tn, TransportKey::Next, now, 900);
    CHECK_EQ(n.mutes, 0);
    CHECK_EQ(n.nexts, 0);
    CHECK(n.seeks.empty());
}

void test_repeat_modes() {
    Queue q;
    q.set({"a", "b", "c"}, 2, "src");
    q.set_repeat(Repeat::Off);
    CHECK_EQ(transport_next_pos(q), -1); /* the last track stays */
    q.set_repeat(Repeat::All);
    CHECK_EQ(transport_next_pos(q), 0); /* wraps */
    q.set_repeat(Repeat::One);
    CHECK_EQ(transport_next_pos(q), 0); /* a press moves on, as the old Next did */
    q.set_current(0);
    for (Repeat r : {Repeat::Off, Repeat::All, Repeat::One}) {
        q.set_repeat(r);
        CHECK_EQ(transport_prev_pos(q), -1); /* the first track restarts in every mode */
    }
    q.set_current(1);
    CHECK_EQ(transport_prev_pos(q), 0);
    CHECK_EQ(transport_next_pos(q), 2);
    Queue empty;
    CHECK_EQ(transport_next_pos(empty), -1);
    CHECK_EQ(transport_prev_pos(empty), -1);
    /* through the module: repeat all wraps on a short Next at the last track */
    FakeHost h;
    h.q.set_current(3);
    h.q.set_repeat(Repeat::All);
    TransportPress t(h);
    uint64_t now = 0;
    hold(t, TransportKey::Next, now, 100);
    CHECK_EQ(h.q.current(), 0);
    h.q.set_current(3);
    h.q.set_repeat(Repeat::Off);
    hold(t, TransportKey::Next, now, 100);
    CHECK_EQ(h.q.current(), 3);
    CHECK_EQ(h.nexts, 1);
}

void test_cancel_during_scan() {
    FakeHost h;
    TransportPress t(h);
    uint64_t now = 0;
    h.pos = 5000;
    t.press(TransportKey::Next, now);
    t.tick(now + 500);
    t.tick(now + 600);
    t.cancel(TransportKey::Next, now + 650);
    CHECK(!h.muted);
    CHECK_EQ(h.pos, static_cast<uint64_t>(5450));
    CHECK_EQ(h.nexts, 0);
}

/* The seek bar's tap and drag (the owner, 2026-11-13: "... so should the track timeline to seek a specific point"), with
 * fake pointer events on a bar 72..408 px (the 480x320 layout): a tap seeks where it lands; a drag shows the target
 * under the finger and seeks once, on the release, to where it ends; past either end the target stops at the end; a
 * press lost seeks nothing; only x counts. */
void test_seek_scrub() {
    CHECK_NEAR(seek_fraction(72, 72, 336), 0.0, 1e-12);
    CHECK_NEAR(seek_fraction(408, 72, 336), 1.0, 1e-12);
    CHECK_NEAR(seek_fraction(240, 72, 336), 0.5, 1e-12);
    CHECK_NEAR(seek_fraction(10, 72, 336), 0.0, 1e-12);  /* left of the bar: its start */
    CHECK_NEAR(seek_fraction(470, 72, 336), 1.0, 1e-12); /* right of it: its end */
    CHECK_NEAR(seek_fraction(100, 0, 0), 0.0, 1e-12);    /* no bar */

    SeekScrub s;
    s.set_bar(72, 336);
    double f = -1;
    /* no press: a release does nothing */
    CHECK(!s.release(200, f));
    CHECK(!s.active());
    /* a tap: press and release on the same spot, one seek there */
    s.press(156); /* a quarter */
    CHECK(s.active());
    CHECK_NEAR(s.target(), 0.25, 1e-12);
    CHECK(s.release(156, f));
    CHECK_NEAR(f, 0.25, 1e-12);
    CHECK(!s.active());
    CHECK(!s.dragged());
    /* a drag: the target follows the finger, the release seeks once, where it ended */
    s.press(100);
    s.move(150);
    CHECK_NEAR(s.target(), (150.0 - 72) / 336, 1e-12);
    s.move(300);
    CHECK_NEAR(s.target(), (300.0 - 72) / 336, 1e-12);
    CHECK(s.dragged());
    CHECK_EQ(s.target_ms(244000), static_cast<uint64_t>((300.0 - 72) / 336 * 244000 + 0.5)); /* the time the label shows */
    CHECK(s.release(324, f));
    CHECK_NEAR(f, 0.75, 1e-12);
    CHECK(!s.release(324, f)); /* no second seek */
    /* past both ends: the target stops at the bar's ends */
    s.press(200);
    s.move(-80);
    CHECK_NEAR(s.target(), 0.0, 1e-12);
    s.move(900);
    CHECK_NEAR(s.target(), 1.0, 1e-12);
    CHECK(s.release(900, f));
    CHECK_NEAR(f, 1.0, 1e-12);
    s.press(400);
    CHECK(s.release(-20, f));
    CHECK_NEAR(f, 0.0, 1e-12);
    /* a press lost (the area deleted, the page changed): nothing */
    s.press(200);
    s.move(260);
    s.cancel();
    CHECK(!s.active());
    CHECK(!s.release(260, f));
    /* moves without a press change nothing */
    s.move(300);
    CHECK(!s.active());
    /* the bar resized (an hour-long track widens the time labels): the target follows the new bar */
    s.set_bar(90, 300);
    s.press(240);
    CHECK_NEAR(s.target(), 0.5, 1e-12);
    s.cancel();
    CHECK_EQ(s.target_ms(0), static_cast<uint64_t>(0));
}

}  // namespace

void test_transport() {
    test_seek_scrub();
    test_short_presses();
    test_no_double_action();
    test_scan_forward();
    test_scan_backward();
    test_scan_end_of_track();
    test_paused_and_no_track();
    test_repeat_modes();
    test_cancel_during_scan();
    CHECK(TransportPress::kRestartAfterMs == 2000);
}
