/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: Knock-knock (owner, 2026-11-16; core/screen): the three quick taps (every press under 0.4 s, every gap
 * under 0.5 s, the three within 1.2 s, the finger within the slop; the edges of each rule, a press elsewhere, a lost press,
 * a run that restarts after a timeout or slides past a slow start), the keys that still act while the screen is off, the
 * gate (Disabled until the setting and the board allow it, the requests it asks for, answers that come late, every way
 * out restoring the light), the app's model fed with touches (arm on the cover only, wake anywhere, nothing swallowed
 * counts twice), the worker thread (order, a waiting request replaced, the exit), the crash guard's raw `screen on`, the
 * setting in settings.json and its row and sentence on the Playback page.
 */
#include "check.hpp"

#include "layout/fit.hpp"
#include "layout/layout.hpp"
#include "menu/menu_model.hpp"
#include "screen/crash_guard.hpp"
#include "screen/knock.hpp"
#include "settings/settings.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vinyl;

namespace {

/* a tap: press at t, release at t + held, at (x, y) and back at (x + dx, y + dy) */
bool tap(TapSequence &s, uint64_t t, uint64_t held = 100, int x = 50, int y = 50, int dx = 0, int dy = 0, bool zone = true) {
    s.press(x, y, t, zone);
    return s.release(x + dx, y + dy, t + held);
}

void test_three_quick_taps() {
    TapSequence s(13);
    CHECK(!tap(s, 1000));
    CHECK_EQ(s.taps(), 1);
    CHECK(!tap(s, 1300));
    CHECK_EQ(s.taps(), 2);
    CHECK(tap(s, 1600)); /* the third release completes it */
    CHECK_EQ(s.taps(), 0); /* and the run is cleared: a fourth tap starts a new one */
    CHECK(!tap(s, 1900));
    CHECK(!tap(s, 2200));
    CHECK(tap(s, 2500));
    /* a release with no press before it */
    TapSequence r;
    CHECK(!r.release(10, 10, 100));
    /* the toolkit's tap slop: a sixth of TARGET, at least 6 px */
    CHECK_EQ(KnockRules::slop_for_target(79), 13);
    CHECK_EQ(KnockRules::slop_for_target(30), 6);
    CHECK_EQ(KnockRules::slop_for_target(0), 6);
}

void test_press_edges() {
    /* a press lasts under 0.4 s: 399 ms counts, 400 ms breaks the run */
    TapSequence s;
    CHECK(!tap(s, 0, 399));
    CHECK(!tap(s, 500, 399)); /* gap 101 */
    CHECK(tap(s, 1000, 199)); /* total 1199 */
    TapSequence l;
    CHECK(!tap(l, 0, 100));
    CHECK(!tap(l, 300, 400)); /* a long press: the run is broken */
    CHECK_EQ(l.taps(), 0);
    CHECK(!tap(l, 800, 100));
    CHECK(!tap(l, 1000, 100));
    CHECK(tap(l, 1200, 100)); /* three new quick taps after it */
}

void test_gap_edges() {
    /* the gap from a release to the next press is under 0.5 s: 499 counts, 500 starts a new run */
    TapSequence s;
    CHECK(!tap(s, 0, 50));      /* released at 50 */
    CHECK(!tap(s, 549, 50));    /* gap 499, released at 599 */
    CHECK(tap(s, 1098, 50));    /* gap 499, total 1148 */
    TapSequence g;
    CHECK(!tap(g, 0, 50));
    CHECK(!tap(g, 550, 50));    /* gap 500: a new run, this is its first tap */
    CHECK_EQ(g.taps(), 1);
    CHECK(!tap(g, 800, 50));
    CHECK(tap(g, 1000, 50));
}

void test_total_edges() {
    /* the three within 1.2 s, from the first press to the last release: 1200 counts, 1201 does not */
    TapSequence s;
    CHECK(!tap(s, 0, 300));
    CHECK(!tap(s, 450, 300));   /* gap 150 */
    CHECK(tap(s, 900, 300));    /* released at 1200: total 1200 */
    TapSequence o;
    CHECK(!tap(o, 0, 300));
    CHECK(!tap(o, 450, 300));
    CHECK(!tap(o, 901, 300));   /* released at 1201: too slow as a run of three */
    CHECK_EQ(o.taps(), 2);      /* the window slid: the last two can still start a run */
    CHECK(tap(o, 1300, 100));   /* 450 .. 1400: three quick taps */
}

void test_slop() {
    TapSequence s(13);
    CHECK(!tap(s, 0, 100, 50, 50, 13, -13)); /* within the slop */
    CHECK(!tap(s, 300, 100, 200, 90, 0, 0)); /* the taps need not be at the same place */
    CHECK(tap(s, 600, 100, 51, 49, -13, 13));
    TapSequence m(13);
    CHECK(!tap(m, 0));
    CHECK(!tap(m, 300, 100, 50, 50, 14, 0)); /* moved 14 px: the run is broken */
    CHECK_EQ(m.taps(), 0);
    /* moved away and back while down: still broken */
    TapSequence b(13);
    CHECK(!tap(b, 0));
    CHECK(!tap(b, 300));
    b.press(50, 50, 600);
    b.move(80, 50);
    b.move(50, 50);
    CHECK(!b.release(50, 50, 700));
    CHECK_EQ(b.taps(), 0);
}

void test_interleaved_and_lost() {
    /* arming: a press outside the cover square breaks the run */
    TapSequence s;
    CHECK(!tap(s, 0));
    CHECK(!tap(s, 200, 100, 300, 300, 0, 0, false)); /* elsewhere */
    CHECK_EQ(s.taps(), 0);
    CHECK(!tap(s, 400));
    CHECK(!tap(s, 600)); /* only two good taps since the wrong one */
    CHECK(tap(s, 800));  /* the third after it completes */
    /* a lost press (the toolkit took it) breaks it */
    TapSequence c;
    CHECK(!tap(c, 0));
    CHECK(!tap(c, 200));
    c.press(50, 50, 400);
    c.cancel();
    CHECK(!c.release(50, 50, 450)); /* no release counts after the cancel */
    CHECK_EQ(c.taps(), 0);
    /* a second press with no release between: starts again */
    TapSequence d;
    CHECK(!tap(d, 0));
    CHECK(!tap(d, 200));
    d.press(50, 50, 400);
    d.press(50, 50, 450);
    CHECK(!d.release(50, 50, 500));
    CHECK_EQ(d.taps(), 1);
}

void test_restart_and_slide() {
    /* two taps, then nothing for 2 s: the run restarts; the next three complete it on their third, not before */
    TapSequence s;
    CHECK(!tap(s, 0));
    CHECK(!tap(s, 200));
    CHECK(!tap(s, 2300));
    CHECK_EQ(s.taps(), 1);
    CHECK(!tap(s, 2500));
    CHECK(tap(s, 2700));
    /* a slow start slides: 0, 550, 1110 is too slow, 550, 1110, 1300 makes it */
    TapSequence w;
    CHECK(!tap(w, 0, 100));
    CHECK(!tap(w, 550, 100));
    CHECK(!tap(w, 1110, 100));
    CHECK(tap(w, 1300, 100));
    /* a steady slow tapping never makes it */
    TapSequence slow;
    bool fired = false;
    for (int i = 0; i < 10; ++i) fired |= tap(slow, static_cast<uint64_t>(i) * 700, 100);
    CHECK(!fired);
    /* nor long presses at a quick pace */
    TapSequence held;
    fired = false;
    for (int i = 0; i < 10; ++i) fired |= tap(held, static_cast<uint64_t>(i) * 450, 420);
    CHECK(!fired);
}

void test_keys() {
    for (uint32_t k : {163u, 164u, 165u, 166u, 200u, 201u}) CHECK(knock_key_kind(k) == KeyKind::Media);
    for (uint32_t k : {113u, 114u, 115u}) CHECK(knock_key_kind(k) == KeyKind::Volume);
    /* - and =, Vinyl's own volume keys, act while off too (owner decision of 2026-11-16) */
    CHECK(knock_key_kind(12) == KeyKind::AppVolume && knock_key_kind(13) == KeyKind::AppVolume);
    /* Esc, Enter, Space, the arrows, letters, digits, comma and dot, the brightness keys */
    for (uint32_t k : {1u, 28u, 57u, 103u, 105u, 106u, 108u, 16u, 50u, 2u, 11u, 51u, 52u, 224u, 225u}) CHECK(knock_key_kind(k) == KeyKind::Other);
}

/* where the setting is offered: the request, `screen` in requests, a touch screen (screen.state touch_slots) */
void test_available() {
    CHECK_EQ(screen_touch_slots("v=1\nw=640\nh=480\ntouch=/dev/input/event0\ntouch_slots=5\n"), 5);
    CHECK_EQ(screen_touch_slots("v=1\nw=480\nh=320\ntouch_slots=0\n"), 0);
    CHECK_EQ(screen_touch_slots("v=1\nw=480\nh=320\ntouch_slots=1\n"), 1);
    CHECK_EQ(screen_touch_slots("v=1\nw=480\nh=320\ntouch=/dev/input/event0\n"), -1); /* an older launcher, multitasking off */
    CHECK_EQ(screen_touch_slots("w=480\nh=320\ntouch_slots=0\n"), -1);                  /* not a v=1 file */
    CHECK_EQ(screen_touch_slots("v=1\ntouch_slots=x\n"), -1);
    CHECK_EQ(screen_touch_slots("v=1\ntouch_slots=-2\n"), -1);
    CHECK_EQ(screen_touch_slots(""), -1);
    CHECK(knock_available(true, true, 5));
    CHECK(knock_available(true, true, -1)); /* unknown: offered */
    CHECK(!knock_available(true, true, 0)); /* no touch screen: no taps, no row */
    CHECK(!knock_available(true, false, 5)); /* the launcher does not answer `screen` */
    CHECK(!knock_available(false, true, 5)); /* the headless harness: no request at hand */
    /* the gate follows: a board found to have no touch screen while off gives the light back */
    Knock k;
    k.set_enabled(true);
    k.set_available(knock_available(true, true, 5));
    CHECK(k.armable());
    k.set_available(knock_available(true, true, 0));
    CHECK(k.state() == GateState::Disabled);
    menu::State st;
    st.knock = knock_available(true, true, 0);
    CHECK_EQ(menu::rows_of(menu::Page::Playback, st).size(), static_cast<size_t>(5));
}

void test_gate() {
    ScreenGate g;
    CHECK(g.state() == GateState::Disabled);
    CHECK(!g.arm().send_off); /* nothing to arm */
    g.set_enabled(true);
    CHECK(g.state() == GateState::Disabled); /* the board does not answer `screen` yet */
    g.set_available(true);
    CHECK(g.state() == GateState::Ready && g.armable());
    CHECK(g.accepts_touch() && g.accepts_key(1) && g.accepts_key(164));
    /* arm: the request, the input ignored at once */
    ScreenGate::Out o = g.arm();
    CHECK(o.send_off && !o.send_on && o.tag == 1);
    CHECK(g.state() == GateState::PendingOff && g.input_blocked());
    CHECK(!g.accepts_touch());
    CHECK(g.accepts_key(164) && g.accepts_key(163) && g.accepts_key(165) && g.accepts_key(115) && g.accepts_key(113));
    CHECK(g.accepts_key(12) && g.accepts_key(13)); /* - and = (Vinyl's volume) */
    CHECK(!g.accepts_key(1) && !g.accepts_key(28) && !g.accepts_key(57) && !g.accepts_key(16) && !g.accepts_key(51));
    CHECK(!g.arm().send_off); /* not twice */
    /* the answer: done -> Off */
    CHECK(!g.result(true, 0, 1).send_on); /* an answer to `on` changes nothing */
    CHECK(g.state() == GateState::PendingOff);
    CHECK(!g.result(false, 0, 7).send_on && g.state() == GateState::PendingOff); /* not this request's answer */
    o = g.result(false, 0, 1);
    CHECK(!o.failed && !o.send_on && g.state() == GateState::Off);
    CHECK(!g.accepts_key(1) && g.accepts_key(164));
    /* wake: Ready, `on` */
    o = g.wake();
    CHECK(o.send_on && !o.send_off && g.state() == GateState::Ready && !g.input_blocked());
    CHECK(!g.wake().send_on); /* nothing to wake */
    /* refused: the screen stays, the input comes back, the notice, `on` for safety */
    o = g.arm();
    CHECK(o.send_off && o.tag == 2);
    o = g.result(false, 1, 2);
    CHECK(o.failed && o.send_on && g.state() == GateState::Ready);
    o = g.arm();
    o = g.result(false, -1, o.tag); /* no service: the same */
    CHECK(o.failed && g.state() == GateState::Ready);
    /* a late answer after a wake changes nothing */
    o = g.arm();
    const uint64_t t = o.tag;
    CHECK(g.wake().send_on);
    o = g.result(false, 0, t);
    CHECK(!o.failed && !o.send_on && g.state() == GateState::Ready);
    /* an older `off`'s answer while a newer one waits changes nothing */
    o = g.arm();
    const uint64_t t1 = o.tag;
    g.wake();
    o = g.arm();
    CHECK(o.tag == t1 + 1);
    CHECK(!g.result(false, 0, t1).send_on && g.state() == GateState::PendingOff);
    CHECK(!g.result(false, 0, o.tag).failed && g.state() == GateState::Off);
    /* every way out restores: exit / hidden */
    o = g.restore();
    CHECK(o.send_on && g.state() == GateState::Ready);
    CHECK(!g.restore().send_on); /* nothing to restore */
    /* the setting switched Off while off: `on`, Disabled */
    g.arm();
    o = g.set_enabled(false);
    CHECK(o.send_on && g.state() == GateState::Disabled && !g.input_blocked());
    CHECK(!g.set_enabled(false).send_on);
    o = g.set_enabled(true);
    CHECK(!o.send_on && g.state() == GateState::Ready);
    /* the launcher stops answering `screen` while going off: `on`, Disabled */
    g.arm();
    o = g.set_available(false);
    CHECK(o.send_on && g.state() == GateState::Disabled);
    CHECK(std::string(gate_state_name(GateState::Off)) == "off");
}

/* the app's model: touches as the view reports them */
struct Finger {
    Knock &k;
    int slop = 13;
    ScreenGate::Out tap(uint64_t t, int x, int y, bool cover, uint64_t held = 80) {
        ScreenGate::Out o = k.touch(TouchPhase::Press, x, y, t, cover);
        const ScreenGate::Out r = k.touch(TouchPhase::Release, x, y, t + held, cover);
        o.send_off |= r.send_off;
        o.send_on |= r.send_on;
        if (r.tag) o.tag = r.tag;
        return o;
    }
};

void test_model() {
    Knock k(13);
    Finger f{k};
    /* Disabled: taps on the cover do nothing */
    CHECK(!f.tap(0, 100, 100, true).send_off);
    CHECK(!f.tap(200, 100, 100, true).send_off);
    CHECK(!f.tap(400, 100, 100, true).send_off);
    k.set_available(true);
    k.set_enabled(true);
    CHECK(k.armable());
    /* three quick taps elsewhere do nothing (arm on the cover only) */
    CHECK(!f.tap(1000, 400, 300, false).send_off);
    CHECK(!f.tap(1200, 400, 300, false).send_off);
    CHECK(!f.tap(1400, 400, 300, false).send_off);
    CHECK(k.armable());
    /* a wrong tap between cover taps breaks the run */
    f.tap(2000, 100, 100, true);
    f.tap(2200, 400, 300, false);
    f.tap(2400, 100, 100, true);
    CHECK(!f.tap(2600, 100, 100, true).send_off);
    /* three quick taps on the cover: off asked, input ignored */
    f.tap(4000, 100, 100, true);
    f.tap(4200, 102, 98, true);
    ScreenGate::Out o = f.tap(4400, 99, 101, true);
    CHECK(o.send_off && o.tag > 0);
    CHECK(k.input_blocked() && !k.accepts_touch());
    CHECK_EQ(k.wake_taps(), 0); /* the arming taps never count for the wake */
    CHECK(!k.accepts_key(1) && !k.accepts_key(57) && k.accepts_key(164) && k.accepts_key(114) && k.accepts_key(12) && k.accepts_key(13));
    o = k.result(false, 0, o.tag);
    CHECK(k.state() == GateState::Off);
    /* while off: a slow tap and a long press wake nothing */
    CHECK(!f.tap(5000, 300, 200, false).send_on);
    CHECK(!f.tap(5700, 300, 200, false).send_on);
    CHECK(!f.tap(6400, 300, 200, false, 450).send_on);
    CHECK(k.state() == GateState::Off);
    /* three quick taps anywhere (not on the cover, at three places): on, input back */
    f.tap(8000, 400, 250, false);
    f.tap(8200, 30, 290, false);
    o = f.tap(8400, 460, 40, true);
    CHECK(o.send_on && !o.send_off);
    CHECK(k.state() == GateState::Ready && k.accepts_touch() && k.accepts_key(1));
    CHECK_EQ(k.arm_taps(), 0); /* the wake taps never count for the next arming */
    CHECK(!f.tap(8600, 100, 100, true).send_off);
    /* off again, then the exit (or hide) restores */
    f.tap(10000, 100, 100, true);
    f.tap(10200, 100, 100, true);
    o = f.tap(10400, 100, 100, true);
    CHECK(o.send_off);
    k.result(false, 0, o.tag);
    o = k.restore();
    CHECK(o.send_on && k.state() == GateState::Ready && !k.input_blocked());
    /* refused: input back at once, the notice */
    f.tap(12000, 100, 100, true);
    f.tap(12200, 100, 100, true);
    o = f.tap(12400, 100, 100, true);
    o = k.result(false, 1, o.tag);
    CHECK(o.failed && k.accepts_touch());
    /* the setting Off while going off: on, Disabled, taps count for nothing */
    f.tap(14000, 100, 100, true);
    f.tap(14200, 100, 100, true);
    CHECK(f.tap(14400, 100, 100, true).send_off);
    o = k.set_enabled(false);
    CHECK(o.send_on && k.state() == GateState::Disabled && k.accepts_touch());
    CHECK(!f.tap(15000, 100, 100, true).send_off);
    /* a press that moved (a drag on the black screen) does not count */
    k.set_enabled(true);
    f.tap(16000, 100, 100, true);
    f.tap(16200, 100, 100, true);
    o = f.tap(16400, 100, 100, true);
    k.result(false, 0, o.tag);
    k.touch(TouchPhase::Press, 200, 200, 17000, false);
    k.touch(TouchPhase::Move, 260, 200, 17040, false);
    k.touch(TouchPhase::Release, 260, 200, 17080, false);
    f.tap(17200, 200, 200, false);
    CHECK(!f.tap(17400, 200, 200, false).send_on);
    CHECK(f.tap(17600, 200, 200, false).send_on);
}

/* a fake cp0_app_backlight that can be held inside a call */
struct FakeBacklight {
    std::mutex mu;
    std::condition_variable cv;
    bool hold = false, inside = false;
    int code = 0;
    std::vector<int> calls;
    int operator()(int on) {
        std::unique_lock<std::mutex> lk(mu);
        inside = true;
        cv.notify_all();
        cv.wait(lk, [&] { return !hold; });
        calls.push_back(on);
        inside = false;
        return code;
    }
    void wait_inside() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return inside; });
    }
    void set_hold(bool h) {
        std::lock_guard<std::mutex> g(mu);
        hold = h;
    }
    void set_code(int c) {
        std::lock_guard<std::mutex> g(mu);
        code = c;
    }
    void let_go() {
        {
            std::lock_guard<std::mutex> g(mu);
            hold = false;
        }
        cv.notify_all();
    }
};

bool wait_idle(BacklightWorker &w) {
    for (int i = 0; i < 500; ++i) {
        if (w.idle()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void test_worker() {
    FakeBacklight fb;
    {
        BacklightWorker w([&fb](int on) { return fb(on); });
        w.request(false, 1);
        CHECK(wait_idle(w));
        BacklightWorker::Result r;
        CHECK(w.take(r));
        CHECK(!r.on && r.code == 0 && r.tag == 1);
        CHECK(!w.take(r));
        /* in order: held inside `off`, an `on` waits behind it */
        fb.set_hold(true);
        w.request(false, 2);
        fb.wait_inside();
        w.request(true);
        fb.let_go();
        CHECK(wait_idle(w));
        CHECK(w.take(r) && !r.on && r.tag == 2);
        CHECK(w.take(r) && r.on);
        /* a request still waiting is replaced by a newer one: off, then quickly on, while busy -> only on is sent */
        fb.set_hold(true);
        w.request(true);
        fb.wait_inside();
        w.request(false, 3);
        w.request(true);
        fb.let_go();
        CHECK(wait_idle(w));
        int ons = 0, offs = 0;
        while (w.take(r)) (r.on ? ons : offs) += 1;
        CHECK_EQ(ons, 2);
        CHECK_EQ(offs, 0);
        CHECK_EQ(w.calls(), 5);
        /* refused */
        fb.set_code(1);
        w.request(false, 4);
        CHECK(wait_idle(w));
        CHECK(w.take(r) && r.code == 1 && r.tag == 4);
        fb.set_code(0);
        /* the exit: stop, then the last `on` at once on this thread */
        w.stop();
        w.request(false, 5); /* after stop: dropped */
        CHECK(w.idle());
        CHECK_EQ(w.call_now(true), 0);
    }
    std::lock_guard<std::mutex> g(fb.mu);
    CHECK_EQ(fb.calls.size(), static_cast<size_t>(7));
    CHECK_EQ(fb.calls.back(), 1);
    /* a worker destroyed with a request waiting joins cleanly */
    FakeBacklight slow;
    slow.set_hold(true);
    {
        BacklightWorker w([&slow](int on) { return slow(on); });
        w.request(false, 1);
        slow.wait_inside();
        w.request(true);
        std::thread t([&slow] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            slow.let_go();
        });
        w.stop();
        t.join();
    }
    std::lock_guard<std::mutex> g2(slow.mu);
    CHECK_EQ(slow.calls.size(), static_cast<size_t>(1)); /* the waiting `on` was dropped: the exit sends it itself */
}

void test_raw_screen_on() {
    CHECK_EQ(screen_on_raw(nullptr), -1);
    CHECK_EQ(screen_on_raw(""), -1);
    CHECK_EQ(screen_on_raw(std::string(200, 'x').c_str()), -1);
    const std::string dir = make_temp_dir("knock");
    const std::string path = dir + "/launcher.sock";
    CHECK_EQ(screen_on_raw(path.c_str()), -1); /* no launcher */
    const int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
    CHECK(::bind(srv, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0);
    CHECK(::listen(srv, 4) == 0);
    CHECK_EQ(screen_on_raw(path.c_str()), 0);
    const int c = ::accept(srv, nullptr, nullptr);
    CHECK(c >= 0);
    char buf[32] = {0};
    const ssize_t n = c >= 0 ? ::read(c, buf, sizeof buf - 1) : -1;
    CHECK_EQ(std::string(buf, n > 0 ? static_cast<size_t>(n) : 0), std::string("screen on\n"));
    if (c >= 0) ::close(c);
    ::close(srv);
    rm_tree(dir);
    /* the guard's flag (the handlers themselves are installed by the app only) */
    CHECK(!crash_guard_armed());
    crash_guard_arm(true);
    CHECK(crash_guard_armed());
    crash_guard_arm(false);
    CHECK(!crash_guard_armed());
}

void test_setting_and_row() {
    /* settings.json: knock_knock, Off by default, kept through a write and a read */
    Settings s;
    CHECK(!s.knock);
    Settings back;
    CHECK(settings_from_json(settings_to_json(s, nullptr), back, nullptr) && !back.knock);
    s.knock = true;
    CHECK(s != Settings());
    const std::string text = settings_to_json(s, nullptr);
    CHECK(text.find("\"knock_knock\"") != std::string::npos);
    CHECK(settings_from_json(text, back, nullptr) && back.knock && back == s);
    CHECK(settings_from_json("{\"knock_knock\": \"yes\"}", back, nullptr) && !back.knock); /* a wrong type: the default */
    CHECK(settings_from_json("{\"gapless\": false}", back, nullptr) && !back.knock);       /* an older file */
    /* the Playback page: the row only where the board answers `screen`, last, On / Off; the strip's sentence with it */
    Settings p;
    std::vector<menu::Row> rows = menu::playback_rows(p);
    CHECK_EQ(rows.size(), static_cast<size_t>(5));
    for (const menu::Row &r : rows) CHECK(r.id != "knock");
    rows = menu::playback_rows(p, "", true);
    CHECK_EQ(rows.size(), static_cast<size_t>(6));
    CHECK(rows.back().id == "knock" && rows.back().text == "Knock-knock" && rows.back().value == "Off");
    CHECK(rows.back().kind == menu::Row::Kind::Edit);
    p.knock = true;
    CHECK_EQ(menu::playback_rows(p, "", true).back().value, std::string("On"));
    menu::State st;
    st.settings = p;
    CHECK_EQ(menu::rows_of(menu::Page::Playback, st).size(), static_cast<size_t>(5)); /* not available: no row, even when On */
    CHECK_EQ(menu::page_hint(menu::Page::Playback, st), std::string("ReplayGain evens out the loudness between tracks."));
    st.knock = true;
    CHECK_EQ(menu::rows_of(menu::Page::Playback, st).size(), static_cast<size_t>(6));
    const std::string hint = menu::page_hint(menu::Page::Playback, st);
    CHECK(hint.find("ReplayGain evens out the loudness between tracks.") == 0);
    CHECK(hint.find(menu::kKnockHint) != std::string::npos);
    CHECK(hint.find("three quick taps on the cover") != std::string::npos);
    /* the selection moves onto it and back */
    const std::vector<menu::Row> pr = menu::rows_of(menu::Page::Playback, st);
    CHECK_EQ(menu::step_item(pr, 4, 1), 5);
    CHECK_EQ(menu::step_item(pr, 5, 1), 5);
}

/* the cover zone (exactly the sleeve) at every QA size: under the header, on the screen, at least the touch target */
void test_cover_zone() {
    struct Q {
        int w, h, body, target;
    };
    const Q sizes[] = {{640, 480, 56, 79}, {640, 480, 56, 63}, {800, 480, 56, 64}, {480, 480, 56, 80}, {720, 720, 56, 70},
                       {480, 800, 56, 60}, {480, 640, 56, 63}, {480, 320, 40, 46}, {320, 480, 40, 44}, {800, 600, 56, 64},
                       {1024, 768, 56, 79}, {1280, 720, 56, 84}, {1024, 600, 56, 79}};
    for (const Q &q : sizes) {
        const layout::NowGeom g = layout::now_geom(layout::size_class(q.w, q.h));
        const vinyl::ui::Fit f = layout::now_fit(g, q.w, q.h, q.body);
        const int x = f.x(g.sleeve.x), y = f.y(g.sleeve.y), side = f.w(g.sleeve.x, g.sleeve.w);
        CHECK(y >= q.body);
        CHECK(x >= 0 && x + side <= q.w && y + side <= q.h);
        CHECK(side >= q.target);
        /* the slop at that size: a sixth of the target */
        CHECK(KnockRules::slop_for_target(q.target) >= 6 && KnockRules::slop_for_target(q.target) * 3 < side);
    }
}

}  // namespace

void test_knock() {
    test_cover_zone();
    test_three_quick_taps();
    test_press_edges();
    test_gap_edges();
    test_total_edges();
    test_slop();
    test_interleaved_and_lost();
    test_restart_and_slide();
    test_keys();
    test_available();
    test_gate();
    test_model();
    test_worker();
    test_raw_screen_on();
    test_setting_and_row();
}
