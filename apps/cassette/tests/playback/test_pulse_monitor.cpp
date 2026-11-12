/* SPDX-License-Identifier: MIT */
/*
 * Cassette live monitor through the sound server (task T3): MonitorTap -> Monitor -> PulsePlaybackSink over FakePulse, with a
 * fake server that plays the stream at its own clock (a "drifting" sink: the capture and the playback streams run on different
 * clocks). Real time, about 2.5 s per case. Synthetic ramps only. The policy under test (monitor.hpp): a slower sink makes the
 * backlog grow and single frames are skipped above target + band; a faster sink runs dry and the underrun is recovered with
 * prefill_ms of silence; the delay never accumulates (no hard resync inside the soft range).
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "monitor.hpp"
#include "pulse_sink.hpp"
#include "support.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

constexpr uint32_t kRate = 8000; /* 20000 frames in 2.5 s still fit the 16-bit ramp */

/* The fake server's playback clock: every 10 ms it plays rate/100 * mult frames of what the stream holds; short of audio after it
 * has started = an underflow. */
struct RtServer {
    FakePulse &f;
    double mult;
    std::atomic<bool> stop{false};
    std::mutex m;
    std::vector<int16_t> data;
    std::atomic<unsigned> underflows{0};
    std::thread th;
    RtServer(FakePulse &p, double mu) : f(p), mult(mu) {
        th = std::thread([this] {
            double acc = 0;
            bool started = false;
            auto next = std::chrono::steady_clock::now();
            while (!stop.load()) {
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
                acc += kRate / 100.0 * mult;
                const size_t want = static_cast<size_t>(acc);
                acc -= static_cast<double>(want);
                std::lock_guard<std::recursive_mutex> g(f.lock());
                for (size_t i = 0; i < f.streams.size(); ++i) {
                    FakeStream &s = f.streams[i];
                    if (s.capture || !s.open || !s.play) continue;
                    std::vector<int16_t> buf(want * s.channels);
                    const size_t got = s.play->take(buf.data(), want);
                    if (got) started = true;
                    {
                        std::lock_guard<std::mutex> g2(m);
                        data.insert(data.end(), buf.begin(), buf.begin() + static_cast<long>(got * s.channels));
                    }
                    if (started && got < want) {
                        s.play->feed_underflow();
                        ++underflows;
                    }
                }
            }
        });
    }
    ~RtServer() {
        stop = true;
        th.join();
    }
    std::vector<int16_t> got() {
        std::lock_guard<std::mutex> g(m);
        return data;
    }
};

struct Run {
    MonitorStats st;
    std::vector<int16_t> played;
    unsigned underflows = 0;
    uint64_t produced = 0;
    int start_rc = 0;
};

/* A producer ticks every 10 ms with kRate/100 * in_mult frames of a rising ramp (the capture clock), after an initial burst. */
Run run_case(double in_mult, double out_mult, size_t burst, double seconds, MonitorOptions opt) {
    FakePulse f;
    f.sinks.push_back(fake_bt_sink("70:BF:92:53:6B:8B", 70));
    f.connect("test", 100);
    Run r;
    {
        RtServer srv(f, out_mult);
        AudioFormat fmt;
        fmt.rate = kRate;
        fmt.channels = 1;
        fmt.bits = 16;
        MonitorTap tap(fmt);
        Monitor mon(tap);
        mon.set_level_pct(100); /* unity gain: the played samples are the ramp itself */
        std::unique_ptr<PulsePlaybackSink> sink(new PulsePlaybackSink(f, pulse_monitor_sink_options()));
        sink->set_sink(f.sinks[0].name);
        r.start_rc = mon.start(std::move(sink), opt);
        if (r.start_rc < 0) return r;
        {
            std::lock_guard<std::recursive_mutex> g(f.lock());
            for (FakeStream &st : f.streams) st.latency_us = 0;
        }
        int16_t next = 1;
        auto put = [&](size_t n) {
            std::vector<int16_t> v(n);
            for (size_t i = 0; i < n; ++i) v[i] = next++;
            tap.write(v.data(), n);
            r.produced += n;
        };
        if (burst) put(burst);
        double acc = 0;
        auto t = std::chrono::steady_clock::now();
        const auto end = t + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
        while (std::chrono::steady_clock::now() < end) {
            t += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(t);
            acc += kRate / 100.0 * in_mult;
            const size_t n = static_cast<size_t>(acc);
            acc -= static_cast<double>(n);
            put(n);
        }
        r.st = mon.stats();
        mon.stop();
        r.played = srv.got();
        r.underflows = srv.underflows.load();
    }
    return r;
}

/* the played samples (the zeros are the silence of the prefill) rise: nothing is reordered or repeated */
bool rising(const std::vector<int16_t> &v) {
    int prev = 0;
    for (int16_t x : v) {
        if (x == 0) continue;
        if (x <= prev) return false;
        prev = x;
    }
    return true;
}

MonitorOptions bt_options() {
    MonitorOptions mo; /* what the controller uses for a Bluetooth sink */
    mo.prefill_ms = 10;
    mo.drift_control = true;
    mo.max_queue_ms = 2000;
    return mo;
}

} // namespace

void test_pulse_monitor() {
    /* the same clocks: audio goes through in order, no skips, no resync, no error */
    {
        Run r = run_case(1.0, 1.0, 0, 2.0, bt_options());
        CHECK_EQ(r.start_rc, 0);
        CHECK(r.st.running);
        CHECK_EQ(r.st.error, 0);
        CHECK(rising(r.played));
        CHECK(r.st.frames_played > 1500);
        CHECK_EQ(r.st.drift_skips, 0u);
        CHECK_EQ(r.st.resyncs, 0u);
        CHECK_EQ(r.st.frames_dropped_in, 0u);
    }
    /* the sink is SLOWER (1 %): the backlog builds up; above target + band single frames are skipped, the hard resync (60 ms) is not
     * needed inside this time, the audio stays in order, and the backlog stays under the hard limit */
    {
        Run r = run_case(1.0, 0.99, 300, 2.5, bt_options());
        CHECK(r.st.running);
        CHECK_EQ(r.st.error, 0);
        CHECK(rising(r.played));
        CHECK(r.st.drift_skips > 5);
        CHECK_EQ(r.st.resyncs, 0u);
        CHECK(r.st.backlog_frames < static_cast<uint64_t>(kMonitorMaxBacklogMs * kRate / 1000.0));
        /* what the sink played + still holds + skipped = what was produced */
        CHECK(r.played.size() + r.st.backlog_frames + r.st.drift_skips <= r.produced + 200);
    }
    /* the same without the drift control: only the hard resync keeps the delay bounded (the ALSA monitor's behaviour) */
    {
        MonitorOptions mo = bt_options();
        mo.drift_control = false;
        Run r = run_case(1.0, 0.99, 300, 2.5, mo);
        CHECK(r.st.running);
        CHECK_EQ(r.st.drift_skips, 0u);
        CHECK(rising(r.played));
    }
    /* the sink is FASTER (2 %): it runs dry; each underrun is recovered with the silence of prefill_ms and the monitor goes on */
    {
        Run r = run_case(1.0, 1.02, 0, 2.5, bt_options());
        CHECK(r.st.running);
        CHECK_EQ(r.st.error, 0);
        CHECK(r.underflows > 0);
        CHECK(r.st.underruns > 0);
        CHECK(r.st.underruns <= r.underflows);
        CHECK_EQ(r.st.resyncs, 0u);
        CHECK(rising(r.played));
        CHECK(r.st.frames_played > 1500); /* it keeps playing through the underruns */
    }
    /* the sink goes away: the monitor thread ends with an error, the tap never blocks (the recording is never affected) */
    {
        FakePulse f;
        f.sinks.push_back(fake_bt_sink("70:BF:92:53:6B:8B", 70));
        f.connect("test", 100);
        RtServer srv(f, 1.0);
        AudioFormat fmt;
        fmt.rate = kRate;
        fmt.channels = 1;
        fmt.bits = 16;
        MonitorTap tap(fmt);
        Monitor mon(tap);
        std::unique_ptr<PulsePlaybackSink> sink(new PulsePlaybackSink(f, pulse_monitor_sink_options()));
        const std::string name = f.sinks[0].name;
        sink->set_sink(name);
        CHECK_EQ(mon.start(std::move(sink), bt_options()), 0);
        std::vector<int16_t> v(80, 100);
        for (int i = 0; i < 20; ++i) {
            tap.write(v.data(), v.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        f.remove_sink(name);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200 && mon.running(); ++i) {
            tap.write(v.data(), v.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!mon.running());
        CHECK(mon.stats().error < 0);
        /* the tap still takes frames at once (a full ring drops, it never blocks) */
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) tap.write(v.data(), v.size());
        CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(200));
        CHECK(t1 - t0 < std::chrono::seconds(3));
        mon.stop();
    }
}
