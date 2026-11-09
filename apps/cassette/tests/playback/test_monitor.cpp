/* SPDX-License-Identifier: MIT */
/*
 * The live monitor unit (monitor.hpp) on fakes: gain curve, tap (lock-free, no allocation, never blocks), mono to stereo,
 * levels 0 / 50 / 100 and a live change, underrun resync, backlog resync, a sink that cannot open, a sink that never returns
 * (capture is not held up, stop() gives up, teardown is safe), and the software latency measured on the fake. Synthetic
 * samples only.
 */
#include "check.hpp"
#include "fake_alsa.hpp"
#include "monitor.hpp"
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

/* the counting allocator of alloc_count.cpp: every allocation of the binary is counted */
long long alloc_count();

using namespace cassette;
using namespace tp;

namespace {

using SC = std::chrono::steady_clock;

double ms_since(SC::time_point t) { return std::chrono::duration<double, std::milli>(SC::now() - t).count(); }

/* A sink the test controls: blocks in write() while `stall` is set, can refuse the rate, and shares its counters with the
 * test (the monitor owns the sink, so the state lives in a shared block). */
struct SinkState {
    std::mutex m;
    std::condition_variable cv;
    bool stall = false;
    unsigned closes = 0, opens = 0, buffering_calls = 0;
    uint32_t asked_period = 0, asked_buffer = 0;
    uint32_t grant_rate = 0;      /* when not 0 the sink grants this rate */
    unsigned grant_channels = 2;
    unsigned write_sleep_us = 0;  /* each write sleeps (a slow device) */
    uint64_t written = 0;
    std::vector<int32_t> heard;   /* interleaved */
    std::vector<std::pair<int32_t, SC::time_point>> stamps; /* first sample of each write, when */
};

class CtlSink final : public PlaybackSink {
public:
    explicit CtlSink(std::shared_ptr<SinkState> s) : s_(std::move(s)) {}
    ~CtlSink() override { close(); }
    void set_buffering(uint32_t p, uint32_t b) override {
        std::lock_guard<std::mutex> g(s_->m);
        ++s_->buffering_calls;
        s_->asked_period = p;
        s_->asked_buffer = b;
    }
    int open(const AudioFormat &src, SinkFormat &g) override {
        std::lock_guard<std::mutex> lk(s_->m);
        ++s_->opens;
        open_ = true;
        g.rate = s_->grant_rate ? s_->grant_rate : src.rate;
        g.channels = s_->grant_channels;
        g.format = SampleFormat::S16_LE;
        g.period_frames = s_->asked_period ? s_->asked_period : kPlaybackPeriod;
        g.buffer_frames = s_->asked_buffer ? s_->asked_buffer : playback_buffer_frames(src.rate);
        ch_ = g.channels;
        return 0;
    }
    long write(const void *f, size_t n) override {
        if (s_->write_sleep_us) std::this_thread::sleep_for(std::chrono::microseconds(s_->write_sleep_us));
        std::unique_lock<std::mutex> lk(s_->m);
        s_->cv.wait(lk, [&] { return !s_->stall; });
        const int16_t *p = static_cast<const int16_t *>(f);
        s_->stamps.emplace_back(n ? p[0] : 0, SC::now());
        for (size_t i = 0; i < n * ch_; ++i) s_->heard.push_back(p[i]);
        s_->written += n;
        return static_cast<long>(n);
    }
    int recover(int e) override { return e; }
    long delay() override { return 0; }
    void drop() override {}
    void drain() override {}
    void close() override {
        std::lock_guard<std::mutex> g(s_->m);
        if (open_) ++s_->closes;
        open_ = false;
    }

private:
    std::shared_ptr<SinkState> s_;
    bool open_ = false;
    unsigned ch_ = 2;
};

/* Forwards to a FakeSink the test owns (the monitor owns the sink it is given). */
class Fwd final : public PlaybackSink {
public:
    explicit Fwd(FakeSink &s) : s_(s) {}
    int open(const AudioFormat &a, SinkFormat &g) override { return s_.open(a, g); }
    long write(const void *f, size_t n) override { return s_.write(f, n); }
    int recover(int e) override { return s_.recover(e); }
    long delay() override { return s_.delay(); }
    void drop() override { s_.drop(); }
    void drain() override { s_.drain(); }
    void close() override { s_.close(); }

private:
    FakeSink &s_;
};

AudioFormat mono() { return afmt(48000, 1, 16); }

std::vector<int16_t> constant(size_t n, int16_t v) { return std::vector<int16_t>(n, v); }

void feed(MonitorTap &t, const std::vector<int16_t> &v) { t.write(v.data(), v.size()); }

/* feed `periods` periods of a constant and wait until the sink holds them */
void feed_const(MonitorTap &t, SinkState &st, int16_t v, size_t frames) {
    uint64_t before;
    {
        std::lock_guard<std::mutex> g(st.m);
        before = st.written;
    }
    const size_t period = 1024;
    /* a capture period every few ms (the ring backlog limit is 60 ms: a burst of 4096 frames would be trimmed) */
    for (size_t done = 0; done < frames; done += period) {
        feed(t, constant(std::min(period, frames - done), v));
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    wait_until(
        [&] {
            std::lock_guard<std::mutex> g(st.m);
            return st.written >= before + frames;
        },
        3000);
}

int32_t last_sample(SinkState &st) {
    std::lock_guard<std::mutex> g(st.m);
    return st.heard.empty() ? 0 : st.heard.back();
}

void t_curve() {
    CHECK_EQ(monitor_gain(0), 0.0f);
    CHECK_EQ(monitor_gain(100), 1.0f);
    CHECK(std::fabs(monitor_gain(50) - 0.25f) < 1e-6f);
    CHECK(std::fabs(monitor_gain(10) - 0.01f) < 1e-6f);
    CHECK_EQ(monitor_gain(-5), 0.0f);
    CHECK_EQ(monitor_gain(250), 1.0f);
    bool mono_up = true;
    for (int p = 1; p <= 100; ++p) mono_up = mono_up && monitor_gain(p) > monitor_gain(p - 1);
    CHECK(mono_up);
    CHECK_EQ(monitor_level_clamp(50), 50);
    CHECK_EQ(monitor_level_clamp(52), 50);
    CHECK_EQ(monitor_level_clamp(53), 55);
    CHECK_EQ(monitor_level_clamp(-7), 0);
    CHECK_EQ(monitor_level_clamp(101), 100);
    CHECK_EQ(monitor_level_clamp(100), 100);
}

void t_tap() {
    MonitorTap tap(mono(), 0.05); /* 2400 frames */
    const std::vector<int16_t> v = constant(1024, 7);
    /* inactive: nothing is kept, everything is reported accepted */
    CHECK_EQ(tap.write(v.data(), v.size()), v.size());
    CHECK_EQ(tap.frames_in(), 0u);
    tap.set_active(true);
    CHECK_EQ(tap.write(v.data(), v.size()), v.size());
    CHECK_EQ(tap.frames_in(), 1024u);
    /* full: the rest is dropped and counted, the call still returns at once with every frame accepted */
    tap.write(v.data(), v.size());
    const long long a0 = alloc_count();
    const auto t0 = SC::now();
    for (int i = 0; i < 1000; ++i) CHECK_EQ(tap.write(v.data(), v.size()), v.size());
    const double took = ms_since(t0);
    CHECK_EQ(alloc_count() - a0, 0LL); /* no allocation on the tap path */
    CHECK(took < 200.0);
    CHECK(tap.dropped() > 1000u * 1024u - 2400u);
    CHECK(tap.ring()->available() <= 2400u);
}

void t_open_and_format() {
    MonitorTap tap(mono());
    Monitor mon(tap);
    auto st = std::make_shared<SinkState>();
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
    CHECK(mon.running());
    CHECK(tap.active());
    CHECK_EQ(st->buffering_calls, 1u);
    CHECK_EQ(st->asked_period, kMonitorPeriodFrames);
    CHECK_EQ(st->asked_buffer, kMonitorBufferFrames);
    const MonitorStats s = mon.stats();
    CHECK_EQ(s.sink_channels, 2u);
    CHECK_EQ(s.sink_buffer, kMonitorBufferFrames);
    CHECK(s.latency_ms > 20.0 && s.latency_ms < 22.0); /* 1024 frames at 48 kHz */
    /* the default level is 50 %: a 8000 sample comes out as 2000, on both channels (mono duplicated) */
    feed_const(tap, *st, 8000, 4096);
    {
        std::lock_guard<std::mutex> g(st->m);
        const size_t n = st->heard.size();
        CHECK(n >= 4096u * 2);
        if (n >= 2) {
            CHECK_EQ(st->heard[n - 1], 2000);
            CHECK_EQ(st->heard[n - 2], 2000);
        }
    }
    mon.stop();
    CHECK(!mon.running());
    CHECK(!tap.active());
    CHECK_EQ(st->closes, 1u);
    mon.stop(); /* twice is fine */
    CHECK_EQ(st->closes, 1u);
}

void t_duplicate_exact() {
    /* full level: the ramp comes out sample for sample, mono duplicated to both channels (FakeSink, stereo only) */
    MonitorTap tap(mono());
    Monitor mon(tap);
    mon.set_level_pct(100);
    FakeSink fs;
    fs.force_channels = 2;
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new Fwd(fs))), 0);
    const auto src = ramp_samples(8192, 1);
    std::vector<int16_t> v;
    for (int32_t x : src) v.push_back(static_cast<int16_t>(x));
    for (size_t i = 0; i < v.size(); i += 1024) {
        tap.write(v.data() + i, 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    CHECK(wait_until([&] { return fs.heard_frames() >= 8192 + kMonitorPrefillFrames; }, 3000));
    const auto heard = fs.heard();
    const size_t lead = kMonitorPrefillFrames; /* the silence the monitor writes first */
    bool exact = heard.size() >= (lead + 8192) * 2;
    for (size_t i = 0; exact && i < lead; ++i) exact = heard[2 * i] == 0 && heard[2 * i + 1] == 0;
    for (size_t i = 0; exact && i < 8192; ++i) exact = heard[2 * (lead + i)] == src[i] && heard[2 * (lead + i) + 1] == src[i];
    CHECK(exact);
    mon.stop();
}

void t_levels() {
    MonitorTap tap(mono());
    Monitor mon(tap);
    auto st = std::make_shared<SinkState>();
    mon.set_level_pct(100);
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
    feed_const(tap, *st, 10000, 4096);
    CHECK_EQ(last_sample(*st), 10000);
    /* a live change, the sink stays open: 50 % -> a quarter, 0 % -> silence, 100 % -> full again */
    mon.set_level_pct(50);
    feed_const(tap, *st, 10000, 4096);
    CHECK_EQ(last_sample(*st), 2500);
    mon.set_level_pct(0);
    feed_const(tap, *st, 10000, 4096);
    CHECK_EQ(last_sample(*st), 0);
    CHECK(mon.running()); /* 0 % keeps the stream alive */
    {
        std::lock_guard<std::mutex> g(st->m);
        CHECK_EQ(st->opens, 1u);
        CHECK_EQ(st->closes, 0u);
    }
    const uint64_t w0 = mon.stats().frames_played;
    mon.set_level_pct(100);
    feed_const(tap, *st, 10000, 4096);
    CHECK_EQ(last_sample(*st), 10000);
    CHECK(mon.stats().frames_played > w0);
    CHECK_EQ(mon.level_pct(), 100);
    /* the ramp: the change is not a step (no click): between the old and the new value the samples rise gradually */
    mon.set_level_pct(0);
    feed_const(tap, *st, 10000, 2048);
    mon.set_level_pct(100);
    const size_t at = [&] {
        std::lock_guard<std::mutex> g(st->m);
        return st->heard.size();
    }();
    feed_const(tap, *st, 10000, 2048);
    {
        std::lock_guard<std::mutex> g(st->m);
        int max_step = 0;
        for (size_t i = at + 2; i < st->heard.size(); i += 2)
            max_step = std::max<int>(max_step, std::abs(st->heard[i] - st->heard[i - 2]));
        CHECK(max_step < 200); /* a step from 0 to 10000 would show as 10000 */
        CHECK_EQ(st->heard.back(), 10000);
    }
    mon.stop();
}

void t_open_failure() {
    MonitorTap tap(mono());
    Monitor mon(tap);
    FakeSink fs;
    fs.open_error = -EBUSY;
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new Fwd(fs))), -EBUSY);
    CHECK(!mon.running());
    CHECK(!tap.active());
    /* a sink that grants another rate cannot play this signal */
    auto st = std::make_shared<SinkState>();
    st->grant_rate = 44100;
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), -EINVAL);
    CHECK(!mon.running() && !tap.active());
    CHECK_EQ(st->closes, 1u);
    CHECK_EQ(mon.start(nullptr), -EINVAL);
    /* and a good start afterwards works */
    auto ok = std::make_shared<SinkState>();
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(ok))), 0);
    CHECK(mon.running());
    mon.stop();
}

void t_underrun_and_death() {
    {
        MonitorTap tap(mono());
        Monitor mon(tap);
        mon.set_level_pct(100);
        FakeSink fs;
        fs.force_channels = 2;
        fs.underrun_at = {2048};
        CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new Fwd(fs))), 0);
        const std::vector<int16_t> v = constant(1024, 5000);
        for (int i = 0; i < 12; ++i) {
            tap.write(v.data(), v.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(wait_until([&] { return fs.heard_frames() >= 4096 + 4096; }, 3000));
        CHECK(mon.running()); /* an underrun: recovered, on we go */
        CHECK_EQ(mon.stats().underruns, 1u);
        mon.stop();
    }
    { /* a lost device ends the thread by itself with the error; the tap is not held */
        MonitorTap tap(mono());
        Monitor mon(tap);
        FakeSink fs;
        fs.lost_at = 1024;
        CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new Fwd(fs))), 0);
        const std::vector<int16_t> v = constant(1024, 5000);
        for (int i = 0; i < 20 && mon.running(); ++i) {
            tap.write(v.data(), v.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(wait_until([&] { return !mon.running(); }, 2000));
        CHECK_EQ(mon.stats().error, -ENODEV);
        const auto t0 = SC::now();
        for (int i = 0; i < 100; ++i) tap.write(v.data(), v.size()); /* still instant */
        CHECK(ms_since(t0) < 100.0);
        mon.stop();
    }
}

void t_backlog_resync() {
    /* a device that takes 25 ms per 256 frames (5.3 ms of audio): the backlog would grow without bound; the monitor drops
     * the oldest audio instead, so what plays stays close to now */
    MonitorTap tap(mono());
    Monitor mon(tap);
    auto st = std::make_shared<SinkState>();
    st->write_sleep_us = 25000;
    mon.set_level_pct(100); /* the counter comes out as it went in */
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
    std::vector<int16_t> v(1024);
    uint64_t produced = 0;
    for (int i = 0; i < 25; ++i) { /* 25 periods: the counter stays under 30000 */
        for (size_t k = 0; k < v.size(); ++k) v[k] = static_cast<int16_t>(produced + k);
        tap.write(v.data(), v.size());
        produced += v.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(21));
    }
    CHECK(mon.stats().resyncs > 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    /* the last chunk written started within ~(backlog + a chunk + the ring) of the newest audio */
    int32_t first = 0;
    {
        std::lock_guard<std::mutex> g(st->m);
        first = st->stamps.empty() ? 0 : st->stamps.back().first;
    }
    const uint64_t lag = produced - static_cast<uint64_t>(first);
    CHECK(lag < 12000u); /* about 250 ms; a queue that kept everything would be about 20000 frames behind */
    CHECK(mon.stats().frames_played < produced);
    mon.stop();
}

void t_stalled_sink() {
    auto tap = std::unique_ptr<MonitorTap>(new MonitorTap(mono()));
    auto mon = std::unique_ptr<Monitor>(new Monitor(*tap));
    auto st = std::make_shared<SinkState>();
    CHECK_EQ(mon->start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
    {
        std::lock_guard<std::mutex> g(st->m);
        st->stall = true; /* the next write never returns */
    }
    /* "the capture thread": 3 s of audio in 1024-frame periods, as fast as it can; every call must come back at once */
    const std::vector<int16_t> v = constant(1024, 3000);
    double worst = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); /* the monitor thread is past its own allocations */
    const long long a0 = alloc_count();
    for (int i = 0; i < 150; ++i) {
        const auto t0 = SC::now();
        const size_t acc = tap->write(v.data(), v.size());
        worst = std::max(worst, ms_since(t0));
        CHECK_EQ(acc, v.size());
        if (i % 10 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(worst < 20.0);
    CHECK_EQ(alloc_count() - a0, 0LL);
    CHECK(tap->dropped() > 0u); /* the ring filled and dropped: the capture side did not wait */
    /* stop() does not wait for ever */
    const auto t1 = SC::now();
    mon->stop();
    const double stop_ms = ms_since(t1);
    CHECK(stop_ms >= kMonitorStopWaitMs - 50.0 && stop_ms < kMonitorStopWaitMs + 1000.0);
    CHECK(mon->stats().stalled_abandoned);
    CHECK(!tap->active());
    /* teardown in the controller's order while the thread is still stuck: monitor, then the tap */
    mon.reset();
    tap.reset();
    {
        std::lock_guard<std::mutex> g(st->m);
        CHECK_EQ(st->closes, 0u); /* the stuck thread still holds the sink */
        st->stall = false;
    }
    st->cv.notify_all();
    CHECK(wait_until(
        [&] {
            std::lock_guard<std::mutex> g(st->m);
            return st->closes == 1u;
        },
        3000)); /* it noticed, let go and closed the sink */
}

void t_teardown_order() {
    for (int round = 0; round < 3; ++round) {
        auto tap = std::unique_ptr<MonitorTap>(new MonitorTap(mono()));
        auto mon = std::unique_ptr<Monitor>(new Monitor(*tap));
        auto st = std::make_shared<SinkState>();
        CHECK_EQ(mon->start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
        const std::vector<int16_t> v = constant(1024, 100);
        for (int i = 0; i < 5; ++i) tap->write(v.data(), v.size());
        mon.reset(); /* the destructor stops: sink closed once, tap inactive */
        {
            std::lock_guard<std::mutex> g(st->m);
            CHECK_EQ(st->closes, 1u);
        }
        CHECK(!tap->active());
        tap->write(v.data(), v.size()); /* harmless */
        tap.reset();
    }
    /* a restart on the same tap: the old audio is not played */
    MonitorTap tap(mono());
    Monitor mon(tap);
    auto s1 = std::make_shared<SinkState>();
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(s1))), 0);
    mon.stop();
    auto s2 = std::make_shared<SinkState>();
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(s2))), 0);
    CHECK(mon.running());
    mon.stop();
}

void t_latency() {
    /* the software delay on the fake: tap -> ring -> thread -> sink, a capture period (1024 frames) every 21.3 ms. The
     * device's own buffer (1024 frames, 21 ms) and the capture period itself come on top. */
    MonitorTap tap(mono());
    Monitor mon(tap);
    mon.set_level_pct(100);
    auto st = std::make_shared<SinkState>();
    CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new CtlSink(st))), 0);
    constexpr int kPeriods = 120;
    std::vector<SC::time_point> at(kPeriods);
    std::vector<int16_t> v(1024);
    const auto t0 = SC::now();
    for (int i = 0; i < kPeriods; ++i) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<long long>(i * 21333.3)));
        for (size_t k = 0; k < v.size(); ++k) v[k] = static_cast<int16_t>(i * 100 + 1000 + static_cast<int>(k / 256));
        at[static_cast<size_t>(i)] = SC::now();
        tap.write(v.data(), v.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<double> lat;
    {
        std::lock_guard<std::mutex> g(st->m);
        for (const auto &s : st->stamps) {
            const int period = (s.first - 1000) / 100;
            if (period >= 0 && period < kPeriods) lat.push_back(std::chrono::duration<double, std::milli>(s.second - at[static_cast<size_t>(period)]).count());
        }
    }
    std::sort(lat.begin(), lat.end());
    CHECK(lat.size() > 300u);
    const double p50 = lat.empty() ? 1e9 : lat[lat.size() / 2];
    const double mx = lat.empty() ? 1e9 : lat.back();
    std::printf("monitor software latency on the fake (tap to sink write, %zu chunks): p50 %.1f ms, max %.1f ms; granted sink buffer %.1f ms\n",
                lat.size(), p50, mx, mon.stats().latency_ms);
    CHECK(p50 < 40.0);
    CHECK_EQ(mon.stats().resyncs, 0u);
    CHECK_EQ(mon.stats().frames_dropped_in, 0u);
    mon.stop();
}

/* The production sink through the fake sound system: the small buffer is asked for, a stereo-only earphone gets the mono
 * microphone twice, and a card that does not play the capture rate is opened through plughw: exactly like the Player. */
void t_alsa_sink() {
    {
        FakeAlsa fa;
        fa.cards_ = {fake_uc02(0)};
        MonitorTap tap(mono());
        Monitor mon(tap);
        mon.set_level_pct(100);
        CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new AlsaPlaybackSink(fa, "hw:CARD=UC02,DEV=0"))), 0);
        CHECK_EQ(fa.opened_playback.size(), 1u);
        CHECK_EQ(fa.opened_playback.empty() ? std::string() : fa.opened_playback[0], std::string("hw:CARD=UC02,DEV=0"));
        CHECK(!fa.playback_requests.empty());
        if (!fa.playback_requests.empty()) {
            CHECK_EQ(fa.playback_requests[0].period_frames, kMonitorPeriodFrames);
            CHECK_EQ(fa.playback_requests[0].buffer_frames, kMonitorBufferFrames);
            CHECK_EQ(fa.playback_requests[0].channels, 2u); /* stereo-only earphones */
        }
        CHECK_EQ(mon.stats().sink_channels, 2u);
        std::vector<int16_t> v(1024);
        for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(i * 10);
        for (int k = 0; k < 4; ++k) {
            tap.write(v.data(), v.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
        mon.stop(); /* the fake is not thread-safe: read it after the thread ended */
        CHECK(fa.played.size() >= 1024 * 4);
        bool dup = fa.played.size() >= 4096;
        for (size_t i = 0; dup && i + 3 < 4096; i += 4) dup = fa.played[i] == fa.played[i + 2] && fa.played[i + 1] == fa.played[i + 3];
        CHECK(dup);
    }
    { /* the playback side does not offer 48000: plughw: converts the rate, as playback does */
        FakeAlsa fa;
        FakeCard c = fake_pcm2902(0);
        c.play_offer.rates = {44100};
        fa.cards_ = {c};
        MonitorTap tap(mono());
        Monitor mon(tap);
        CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new AlsaPlaybackSink(fa, "hw:CARD=Device,DEV=0"))), 0);
        CHECK_EQ(fa.opened_playback.empty() ? std::string() : fa.opened_playback.back(), std::string("plughw:CARD=Device,DEV=0"));
        mon.stop();
    }
    { /* busy / refused: the error comes back, the monitor stays off */
        FakeAlsa fa;
        FakeCard c = fake_pcm2902(0);
        c.play_open_error = -EBUSY;
        fa.cards_ = {c};
        MonitorTap tap(mono());
        Monitor mon(tap);
        CHECK_EQ(mon.start(std::unique_ptr<PlaybackSink>(new AlsaPlaybackSink(fa, "hw:CARD=Device,DEV=0"))), -EBUSY);
        CHECK(!mon.running() && !tap.active());
    }
}

} // namespace

void test_monitor() {
    t_alsa_sink();
    t_curve();
    t_tap();
    t_open_and_format();
    t_duplicate_exact();
    t_levels();
    t_open_failure();
    t_underrun_and_death();
    t_backlog_resync();
    t_stalled_sink();
    t_teardown_order();
    t_latency();
}
