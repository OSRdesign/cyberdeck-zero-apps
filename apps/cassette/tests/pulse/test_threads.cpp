/*
 * SPDX-License-Identifier: MIT
 *
 * Threads and shutdown (run under TSan: tests/pulse/run.sh SAN=thread): a server thread feeding a capture stream while
 * the owner reads; close() racing the server and waking a blocked reader; no change callback after disconnect(); a
 * playback writer and a consuming server; an end arriving while the reader drains.
 */
#include "check.hpp"
#include "fake_pulse.hpp"

#include <atomic>
#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";

std::string evolve() { return fake_bt_source(kEvolve).name; }

void setup(FakePulse &f) {
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.sinks.push_back(fake_bt_sink(kEvolve, 70));
    f.connect("Cassette", 1000);
}

uint64_t frames_in_locked(FakePulse &f, PulseCapture &c) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    return c.stats().frames_in;
}

void test_capture_threads() {
    FakePulse f;
    setup(f);
    PulseCaptureRequest r;
    r.source = evolve();
    r.ring_ms = 1000;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    if (!cap) return;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sent{0};
    std::thread server([&] {
        std::vector<int16_t> chunk(160);
        uint32_t k = 0;
        while (!stop.load()) {
            for (auto &s : chunk) s = static_cast<int16_t>((k++ & 0x7FFF) | 1);
            if (f.deliver(0, chunk.data(), chunk.size())) sent += chunk.size();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    /* the owner reads, checks the pattern is continuous */
    std::vector<int16_t> buf(1024);
    uint64_t got = 0;
    uint32_t expect = 0;
    bool continuous = true;
    while (got < 16000) {
        const long n = cap->read(buf.data(), buf.size(), 200);
        if (n < 0) break;
        for (long i = 0; i < n; ++i) {
            continuous &= buf[static_cast<size_t>(i)] == static_cast<int16_t>((expect++ & 0x7FFF) | 1);
        }
        got += static_cast<uint64_t>(n);
    }
    CHECK(got >= 16000);
    CHECK(continuous);
    CHECK_EQ(cap->stats().frames_overflow, 0u);
    /* events polled on another thread while the server feeds */
    std::thread poller([&] {
        PulseStreamEvent ev;
        for (int i = 0; i < 200; ++i) {
            cap->poll_event(ev);
            (void)cap->state();
            (void)cap->stats();
        }
    });
    poller.join();
    cap->close(); /* while the server thread is still delivering */
    const uint64_t at_close = frames_in_locked(f, *cap);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(frames_in_locked(f, *cap), at_close); /* nothing reaches a closed stream */
    stop = true;
    server.join();
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK(f.streams[0].delivered_after_close > 0);
        /* every frame the server handed over is accounted for: taken in, or discarded while the close ran */
        const PulseCaptureStats st = cap->stats();
        CHECK_EQ(st.frames_in + st.frames_after_end, sent.load());
    }
}

void test_close_wakes_reader() {
    FakePulse f;
    setup(f);
    PulseCaptureRequest r;
    r.source = evolve();
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    std::atomic<long> res{1};
    const auto t0 = std::chrono::steady_clock::now();
    std::thread reader([&] {
        std::vector<int16_t> buf(256);
        res = cap->read(buf.data(), 256, 5000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    cap->close();
    reader.join();
    CHECK_EQ(res.load(), static_cast<long>(kPulseState));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));

    /* an end arriving while the reader waits: the frames first, then the end code */
    std::unique_ptr<PulseCapture> c2;
    CHECK_EQ(f.open_capture(r, c2, nullptr), 0);
    std::vector<long> results;
    std::thread rd([&] {
        std::vector<int16_t> buf(4096);
        for (;;) {
            const long n = c2->read(buf.data(), buf.size(), 2000);
            results.push_back(n);
            if (n < 0) break;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    f.deliver_value(1, 5, 800);
    f.remove_source(evolve());
    rd.join();
    long total = 0;
    for (size_t i = 0; i + 1 < results.size(); ++i) total += results[i];
    CHECK_EQ(total, 800L);
    CHECK(!results.empty() && results.back() == kPulseKilled);
}

void test_no_callback_after_disconnect() {
    FakePulse f;
    setup(f);
    std::atomic<unsigned> calls{0};
    f.set_change_callback([&](const PulseChange &) { ++calls; });
    std::atomic<bool> stop{false};
    std::thread churn([&] {
        unsigned i = 0;
        while (!stop.load()) {
            PulseDevice d = fake_bt_source("AA:BB:CC:DD:EE:0" + std::to_string(i++ % 10));
            f.add_source(d);
            f.remove_source(d.name);
        }
    });
    CHECK(wait_until([&] { return calls.load() > 50; }, 2000));
    f.disconnect();
    const unsigned at = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(calls.load(), at);
    stop = true;
    churn.join();
    CHECK_EQ(calls.load(), at);

    /* set_change_callback(nullptr) is the same barrier */
    FakePulse g;
    setup(g);
    std::atomic<unsigned> c2{0};
    g.set_change_callback([&](const PulseChange &) { ++c2; });
    stop = false;
    std::thread churn2([&] {
        while (!stop.load()) g.add_source(fake_bt_source("AA:BB:CC:DD:EE:FF"));
    });
    CHECK(wait_until([&] { return c2.load() > 20; }, 2000));
    g.set_change_callback(nullptr);
    const unsigned at2 = c2.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(c2.load(), at2);
    stop = true;
    churn2.join();
}

void test_playback_threads() {
    FakePulse f;
    setup(f);
    PulsePlaybackRequest r;
    r.sink = fake_bt_sink(kEvolve).name;
    r.rate = 16000;
    r.channels = 1;
    r.buffer_ms = 40;
    std::unique_ptr<PulsePlayback> p;
    CHECK_EQ(f.open_playback(r, p, nullptr), 0);
    if (!p) return;
    std::atomic<bool> stop{false};
    std::vector<int16_t> heard;
    std::thread server([&] {
        std::vector<int16_t> part;
        while (!stop.load()) {
            part.clear();
            f.consume(0, 160, &part);
            heard.insert(heard.end(), part.begin(), part.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::vector<int16_t> chunk(320);
    int16_t k = 1;
    long written = 0;
    for (int i = 0; i < 50; ++i) {
        for (auto &s : chunk) s = k++;
        size_t off = 0;
        while (off < chunk.size()) {
            const long n = p->write(chunk.data() + off, chunk.size() - off, 500);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
            written += n;
        }
    }
    CHECK_EQ(written, 50L * 320L);
    CHECK(wait_until([&] { return p->stats().frames_to_server == static_cast<uint64_t>(written); }, 3000));
    stop = true;
    server.join();
    CHECK_EQ(heard.size(), static_cast<size_t>(written));
    bool in_order = true;
    for (size_t i = 0; i < heard.size(); ++i) in_order &= heard[i] == static_cast<int16_t>(i + 1);
    CHECK(in_order);

    /* close() wakes a writer waiting for room */
    std::vector<int16_t> big(1600, 1);
    while (p->write(big.data(), big.size(), 0) > 0) {
    }
    std::atomic<long> res{1};
    std::thread w([&] { res = p->write(big.data(), big.size(), 5000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p->close();
    w.join();
    CHECK_EQ(res.load(), static_cast<long>(kPulseState));
}

}  // namespace

void test_threads() {
    test_capture_threads();
    test_close_wakes_reader();
    test_no_callback_after_disconnect();
    test_playback_threads();
}
