/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the live monitor in the controller (settings monitor / monitor_level). The monitor sink and the
 * player's sink are told apart by set_buffering(), which only the monitor calls; a log with every open and close of both,
 * in order, shows that the monitor lets go of the card BEFORE the Player takes it. Synthetic tone only.
 */
#include "helpers.hpp"

using namespace ta;

namespace {

/* one log for both sinks: "mon open", "mon close", "play open", "play close" */
struct SinkLog {
    std::mutex m;
    std::vector<std::string> ev;
    std::atomic<bool> stall{false};     /* the monitor sink's write does not return */
    std::atomic<unsigned> mon_opens{0}, mon_closes{0}, play_opens{0};
    std::vector<std::string> devices;   /* the device string of every sink asked for */
    void add(const std::string &e) {
        std::lock_guard<std::mutex> g(m);
        ev.push_back(e);
    }
    std::vector<std::string> events() {
        std::lock_guard<std::mutex> g(m);
        return ev;
    }
    long index_of(const std::string &e, size_t from = 0) {
        std::lock_guard<std::mutex> g(m);
        for (size_t i = from; i < ev.size(); ++i)
            if (ev[i] == e) return static_cast<long>(i);
        return -1;
    }
};

class RoleSink final : public PlaybackSink {
public:
    RoleSink(SinkLog &log, tp::FakeSink &mon, tp::FakeSink &play) : log_(log), mon_(mon), play_(play) {}
    void set_buffering(uint32_t, uint32_t) override { monitor_ = true; }
    int open(const AudioFormat &src, SinkFormat &g) override {
        const int r = cur().open(src, g);
        if (r == 0) {
            open_ = true;
            if (monitor_) ++log_.mon_opens;
            else ++log_.play_opens;
            log_.add(monitor_ ? "mon open" : "play open");
        } else {
            log_.add(monitor_ ? "mon open failed" : "play open failed");
        }
        return r;
    }
    long write(const void *f, size_t n) override {
        if (monitor_)
            for (int i = 0; log_.stall.load() && i < 20000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return cur().write(f, n);
    }
    int recover(int e) override { return cur().recover(e); }
    long delay() override { return cur().delay(); }
    void drop() override { cur().drop(); }
    void drain() override { cur().drain(); }
    void close() override {
        if (open_) {
            open_ = false;
            if (monitor_) ++log_.mon_closes;
            log_.add(monitor_ ? "mon close" : "play close");
        }
        cur().close();
    }

private:
    tp::FakeSink &cur() { return monitor_ ? mon_ : play_; }
    SinkLog &log_;
    tp::FakeSink &mon_, &play_;
    bool monitor_ = false, open_ = false;
};

struct MRig : Rig {
    SinkLog log;
    tp::FakeSink msink;
    explicit MRig(std::vector<FakeCard> cards, const std::string &home = "") : Rig(std::move(cards), home) {
        cfg.make_sink = [this](const std::string &dev) {
            {
                std::lock_guard<std::mutex> g(log.m);
                log.devices.push_back(dev);
            }
            return std::unique_ptr<PlaybackSink>(new RoleSink(log, msink, sink));
        };
    }
    ~MRig() { c.reset(); } /* the controller (and its monitor thread) goes before the sinks it writes to */
    bool monitor_active() { return snap().card.monitor_active; }
    bool wait_mon(bool on, int ms = 3000) {
        return wait([on](const Snapshot &s) { return s.card.monitor_active == on; }, ms);
    }
    int32_t peak_since(size_t from) { /* the largest |sample| the monitor sink got after sample `from` */
        const auto h = msink.heard();
        int32_t p = 0;
        for (size_t i = from; i < h.size(); ++i) p = std::max<int32_t>(p, std::abs(h[i]));
        return p;
    }
};


const char *kTake = "take_001_20261108-1105.wav";

int count_notice(const Snapshot &s, const std::string &text) {
    int n = 0;
    for (const Notice &x : s.notices)
        if (x.text == text) ++n;
    return n;
}

void t_defaults_rows_failure() {
    {
        MRig r(one_card());
        r.open();
        CHECK(r.wait_meter());
        Snapshot s = r.snap();
        CHECK(!s.settings.monitor); /* off by default */
        CHECK_EQ(s.settings.monitor_level, 50);
        CHECK(s.card.has_playback); /* this card has a playback side: the rows would show */
        CHECK(!s.card.monitor_active);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_EQ(r.log.mon_opens.load(), 0u); /* nothing opened while it is off */
        CHECK(!has_notice(r.snap(), Notice::Info));
    }
    { /* a capture-only card: no playback side, no rows, and switching it on does nothing (no notice either) */
        MRig r({fake_card_no_controls(0)});
        r.open();
        CHECK(r.wait([](const Snapshot &x) { return x.card.present; }));
        CHECK(!r.snap().card.has_playback);
        CHECK(r.ctl().set_monitor(true));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!r.snap().card.monitor_active);
        CHECK_EQ(r.log.mon_opens.load(), 0u);
        CHECK(!has_notice(r.snap(), Notice::Info));
    }
    { /* the sink cannot open (EBUSY): off, one short notice, no retry loop, recording untouched; a toggle tries again */
        MRig r(one_card());
        r.msink.open_error = -EBUSY;
        r.open();
        Controller &c = r.ctl();
        CHECK(r.wait_meter());
        CHECK(c.set_monitor(true));
        CHECK(!r.snap().card.monitor_active);
        CHECK_EQ(count_notice(r.snap(), "Monitor unavailable"), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ(count_notice(r.snap(), "Monitor unavailable"), 1); /* once, not every tick */
        CHECK_EQ(r.log.mon_opens.load(), 0u);
        c.key_event(KeyEvent::Rec);
        CHECK(r.wait_state(kDeckRecording));
        CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 0.5; }));
        c.key_event(KeyEvent::Stop);
        CHECK(r.wait_state(kDeckIdle));
        CHECK_EQ(r.snap().problems.frames_lost, 0u);
        CHECK_EQ(r.snap().problems.frames_dropped, 0u);
        CHECK_EQ(c.takes().size(), 1u);
        /* the card is free again: switching it off and on tries again and works */
        r.msink.open_error = 0;
        CHECK(c.set_monitor(false));
        CHECK(c.set_monitor(true));
        CHECK(r.wait_mon(true));
    }
}

void t_live_and_levels() {
    MRig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_monitor(true));
    CHECK(r.wait_mon(true));
    CHECK_EQ(r.log.mon_opens.load(), 1u);
    {
        std::lock_guard<std::mutex> g(r.log.m);
        CHECK_EQ(r.log.devices.size(), 2u); /* the Player's sink at start, the monitor's */
        if (!r.log.devices.empty())
            CHECK_EQ(r.log.devices.back(), std::string("hw:CARD=Device,DEV=0")); /* the capture card's own playback side */
    }
    /* 50 %: a quarter of the tone (0.5 full scale: about 4096) */
    CHECK(wait_until([&] { return r.msink.heard_frames() > 4800; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    size_t at = r.msink.heard().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int32_t p50 = r.peak_since(at);
    CHECK(p50 > 3500 && p50 < 4500);
    /* live level changes: no reopen */
    CHECK(c.set_monitor_level(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    at = r.msink.heard().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int32_t p100 = r.peak_since(at);
    CHECK(p100 > 15500 && p100 < 17000);
    CHECK(c.set_monitor_level(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    at = r.msink.heard().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(r.peak_since(at), 0);
    CHECK(r.monitor_active());
    CHECK_EQ(r.log.mon_opens.load(), 1u);
    CHECK_EQ(r.log.mon_closes.load(), 0u);
    CHECK(c.set_monitor_level(37)); /* steps of 5 */
    CHECK_EQ(r.snap().settings.monitor_level, 35);
    CHECK(c.set_monitor_level(38));
    CHECK_EQ(r.snap().settings.monitor_level, 40);
    CHECK(c.set_monitor_level(500));
    CHECK_EQ(r.snap().settings.monitor_level, 100);
    CHECK(c.set_monitor_level(-3));
    CHECK_EQ(r.snap().settings.monitor_level, 0);
    /* off: the sink closes at once */
    CHECK(c.set_monitor(false));
    CHECK(r.wait_mon(false));
    CHECK_EQ(r.log.mon_closes.load(), 1u);
}

void t_transitions() {
    MRig r(one_card());
    make_take(r.rec(), kTake, 6.0);
    r.sink.sleep_us = 8000;
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_record_timer(0));
    CHECK(c.set_monitor(true));
    CHECK(c.set_monitor_level(100));
    CHECK(r.wait_mon(true));
    unsigned opens = r.log.mon_opens.load();

    /* idle -> playing: the monitor is closed BEFORE the Player opens its sink */
    c.key_event(KeyEvent::PlayPause);
    CHECK(r.wait_state(kDeckPlaying));
    CHECK(r.wait_mon(false));
    {
        const long mc = r.log.index_of("mon close");
        const long po = r.log.index_of("play open");
        CHECK(mc >= 0 && po >= 0 && mc < po);
    }
    /* playing -> paused -> playing: still off */
    c.key_event(KeyEvent::PlayPause);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!r.monitor_active());
    c.key_event(KeyEvent::PlayPause);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlaying));
    /* -> scan forward (held), then reverse */
    c.key_event(KeyEvent::FwdDown);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckScanningForward));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!r.monitor_active());
    c.key_event(KeyEvent::FwdUp);
    c.key_event(KeyEvent::RewDown);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckScanningReverse));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!r.monitor_active());
    c.key_event(KeyEvent::RewUp);
    CHECK(!r.monitor_active());
    CHECK_EQ(r.log.mon_opens.load(), opens);
    /* stop: the monitor is back */
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.wait_mon(true));
    CHECK_EQ(r.log.mon_opens.load(), opens + 1);

    /* a jump-mode cue from idle (PlayingPaused): off, back after stop */
    CHECK(c.set_scan_mode(ScanMode::Jump));
    c.key_event(KeyEvent::FwdDown);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckPlayingPaused));
    CHECK(r.wait_mon(false));
    c.key_event(KeyEvent::FwdUp);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.wait_mon(true));
    CHECK(c.set_scan_mode(ScanMode::Fast));

    /* the list's preview is playback too */
    CHECK(c.preview_take(kTake));
    CHECK(r.wait_mon(false));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.wait_mon(true));

    /* countdown: the recorder screen, the monitor runs; cancelling keeps it; REC while playing brings it back */
    CHECK(c.set_record_timer(3));
    c.key_event(KeyEvent::Rec);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(r.monitor_active());
    c.key_event(KeyEvent::TapOverlay);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(r.monitor_active());
    c.key_event(KeyEvent::PlayPause);
    CHECK(r.wait_mon(false));
    c.key_event(KeyEvent::Rec); /* playing -> countdown */
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckCountdown));
    CHECK(r.wait_mon(true));
    c.key_event(KeyEvent::TapOverlay);
    CHECK(r.wait_state(kDeckIdle));
    CHECK(c.set_record_timer(0));

    /* recording and paused recording: it runs; switching it off and on in the middle does not glitch the take */
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.monitor_active());
    c.key_event(KeyEvent::PlayPause);
    CHECK_EQ(r.snap().deck.state, static_cast<int>(kDeckRecordingPaused));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(r.monitor_active());
    c.key_event(KeyEvent::PlayPause);
    CHECK(r.monitor_active());
    for (int i = 0; i < 4; ++i) {
        CHECK(c.set_monitor(i % 2 == 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    CHECK(r.monitor_active());
    CHECK(c.set_monitor_level(60));
    CHECK(c.set_monitor_level(80));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 1.5; }));
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    Snapshot s = r.snap();
    CHECK_EQ(s.problems.frames_lost, 0u);
    CHECK_EQ(s.problems.frames_dropped, 0u);
    CHECK_EQ(s.problems.overruns, 0u);
    CHECK(!s.problems.write_error);
    CHECK(r.wait_mon(true));
    std::vector<std::string> w = r.wavs();
    CHECK_EQ(w.size(), 2u);
    for (const std::string &n : w) {
        if (n == kTake) continue;
        WavData d = read_wav(r.rec() + "/" + n);
        CHECK(d.ok && d.consistent);
        CHECK(d.frames() > 48000u * 14 / 10);
        CHECK(d.frames() < 48000u * 4);
    }

    /* the trim page: capture goes on but the monitor stays off while it is open */
    CHECK(c.open_trim(kTake));
    CHECK(r.wait_mon(false));
    c.close_trim();
    CHECK(r.wait_mon(true));

    /* the card is pulled: closed, no crash; back with the card */
    r.alsa.unplug("Device");
    CHECK(r.wait([](const Snapshot &x) { return !x.card.present; }, 3000));
    CHECK(!r.monitor_active());
    CHECK_EQ(r.log.mon_closes.load(), r.log.mon_opens.load());
    r.alsa.replug("Device");
    CHECK(r.wait([](const Snapshot &x) { return x.card.present; }, 4000));
    CHECK(r.wait_mon(true));

    /* leaving the app (Esc on the recorder) */
    c.app_leave();
    CHECK(r.wait_mon(false));
    CHECK_EQ(r.log.mon_closes.load(), r.log.mon_opens.load());
    r.close();
    CHECK_EQ(r.log.mon_closes.load(), r.log.mon_opens.load());
}

void t_stalled_sink_does_not_touch_the_take() {
    MRig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    r.log.stall = true; /* the monitor's device never takes another frame */
    CHECK(c.set_monitor(true));
    CHECK(r.wait_mon(true));
    c.key_event(KeyEvent::Rec);
    CHECK(r.wait_state(kDeckRecording));
    CHECK(r.wait([](const Snapshot &x) { return x.time.elapsed_s >= 1.5; }, 4000));
    Snapshot s = r.snap();
    CHECK_EQ(s.problems.frames_lost, 0u);
    CHECK_EQ(s.problems.frames_dropped, 0u);
    CHECK_EQ(s.problems.overruns, 0u);
    CHECK(s.card.present);
    c.key_event(KeyEvent::Stop);
    CHECK(r.wait_state(kDeckIdle));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(c.set_monitor(false)); /* waits a moment for the stuck thread, then gives up on it */
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.0);
    CHECK(r.wait_mon(false));
    std::vector<TakeRow> rows = c.takes();
    CHECK_EQ(rows.size(), 1u);
    if (!rows.empty()) {
        WavData w = read_wav(rows[0].path);
        CHECK(w.ok && w.consistent);
        CHECK(std::fabs(static_cast<double>(w.frames()) / 48000.0 - rows[0].seconds) < 0.05);
        CHECK(rows[0].seconds >= 1.5 && rows[0].seconds < 3.5);
        CHECK(!rows[0].damaged);
    }
    r.log.stall = false; /* the stuck thread wakes, sees it was told to stop and closes its sink */
    CHECK(wait_until([&] { return r.log.mon_closes.load() == 1u; }, 3000));
}

void t_persistence() {
    MRig r(one_card());
    r.open();
    Controller &c = r.ctl();
    CHECK(r.wait_meter());
    CHECK(c.set_monitor(true));
    CHECK(c.set_monitor_level(35));
    CHECK(r.wait_mon(true));
    const std::string file = slurp(r.home + "/.config/cassette/settings.json");
    CHECK(file.find("\"monitor\"") != std::string::npos);
    CHECK(file.find("\"monitor_level\"") != std::string::npos);
    r.close();

    MRig r2(one_card(), r.home);
    r2.open();
    CHECK(r2.wait_meter());
    Snapshot s = r2.snap();
    CHECK(s.settings.monitor);
    CHECK_EQ(s.settings.monitor_level, 35);
    CHECK(r2.wait_mon(true)); /* a saved On comes back with the capture path */
    CHECK(r2.ctl().set_monitor(false));
    r2.close();

    MRig r3(one_card(), r.home);
    r3.open();
    CHECK(r3.wait_meter());
    CHECK(!r3.snap().settings.monitor);
    CHECK_EQ(r3.snap().settings.monitor_level, 35);
    r3.close();
}

} // namespace

void test_monitor_app() {
    t_defaults_rows_failure();
    t_live_and_levels();
    t_transitions();
    t_stalled_sink_does_not_touch_the_take();
    t_persistence();
}
