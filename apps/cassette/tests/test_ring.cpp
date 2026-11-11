/*
 * SPDX-License-Identifier: MIT
 *
 * Ring: capacity, exact fit, full ring, wrap-around, two threads under load.
 */
#include "check.hpp"
#include "ring.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace cassette;

static void basics() {
    Ring r(AudioFormat{}); /* 48 kHz mono S16, 8 s */
    CHECK_EQ(r.frame_bytes(), static_cast<size_t>(2));
    CHECK_EQ(r.capacity_frames(), static_cast<size_t>(384000));
    CHECK_EQ(r.capacity_frames() * r.frame_bytes(), static_cast<size_t>(768000));
    CHECK_EQ(r.available(), static_cast<size_t>(0));
    int16_t in[5] = {1, 2, 3, 4, 5}, out[8] = {};
    CHECK_EQ(r.write(in, 5), static_cast<size_t>(5));
    CHECK_EQ(r.available(), static_cast<size_t>(5));
    CHECK_EQ(r.read(out, 8), static_cast<size_t>(5));
    CHECK(out[0] == 1 && out[4] == 5);
    CHECK_EQ(r.read(out, 8), static_cast<size_t>(0));
    CHECK_EQ(r.frames_written(), static_cast<uint64_t>(5));
    CHECK_EQ(r.frames_read(), static_cast<uint64_t>(5));
}

static void exact_fit_and_full() {
    AudioFormat f;
    f.rate = 1000;
    Ring r(f, 1.0); /* 1000 frames */
    std::vector<int16_t> a(1000);
    for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<int16_t>(i);
    CHECK_EQ(r.write(a.data(), 1000), static_cast<size_t>(1000)); /* exact fit */
    CHECK_EQ(r.frames_dropped(), static_cast<uint64_t>(0));
    CHECK(r.fill_ratio() == 1.0);
    CHECK_EQ(r.write(a.data(), 7), static_cast<size_t>(0)); /* full */
    CHECK_EQ(r.frames_dropped(), static_cast<uint64_t>(7));
    std::vector<int16_t> b(1000);
    CHECK_EQ(r.read(b.data(), 1000), static_cast<size_t>(1000));
    CHECK(a == b);
    /* partial acceptance counts the rest as dropped */
    CHECK_EQ(r.write(a.data(), 900), static_cast<size_t>(900));
    CHECK_EQ(r.write(a.data(), 300), static_cast<size_t>(100));
    CHECK_EQ(r.frames_dropped(), static_cast<uint64_t>(7 + 200));
    CHECK_EQ(r.available(), static_cast<size_t>(1000));
}

static void wrap() {
    AudioFormat f;
    f.rate = 100;
    Ring r(f, 1.0); /* 100 frames */
    std::vector<int16_t> a(70), b(60), out(100);
    for (size_t i = 0; i < 70; i++) a[i] = static_cast<int16_t>(i);
    for (size_t i = 0; i < 60; i++) b[i] = static_cast<int16_t>(1000 + i);
    CHECK_EQ(r.write(a.data(), 70), static_cast<size_t>(70));
    CHECK_EQ(r.read(out.data(), 70), static_cast<size_t>(70));
    CHECK_EQ(r.write(b.data(), 60), static_cast<size_t>(60)); /* crosses the end */
    CHECK_EQ(r.read(out.data(), 100), static_cast<size_t>(60));
    bool same = true;
    for (size_t i = 0; i < 60; i++) same = same && out[i] == b[i];
    CHECK(same);
    /* many odd-sized round trips across the wrap */
    uint32_t seq = 0, expect = 0;
    bool ok = true;
    for (int round = 0; round < 500; round++) {
        size_t n = static_cast<size_t>(1 + (round * 7) % 40);
        std::vector<int16_t> w(n);
        for (auto &x : w) x = static_cast<int16_t>(seq++);
        if (r.write(w.data(), n) != n) ok = false;
        std::vector<int16_t> rd(64);
        size_t got = r.read(rd.data(), 64);
        for (size_t i = 0; i < got; i++) ok = ok && rd[i] == static_cast<int16_t>(expect++);
    }
    CHECK(ok);
    CHECK_EQ(r.frames_dropped(), static_cast<uint64_t>(0));
}

/* Two threads: frames are 2 x uint32 {n, n * 2654435761}; the consumer checks order and content, and the
 * accounting: offered == read + dropped (+ still in the ring at the end). */
static void two_threads(double seconds, bool slow_consumer, uint64_t total) {
    AudioFormat f;
    f.rate = 48000;
    f.channels = 2;
    f.bits = 32;
    Ring r(f, seconds);
    std::atomic<bool> done{false};
    uint64_t offered = 0;
    std::thread prod([&] {
        uint32_t n = 0;
        uint32_t rng = 12345;
        std::vector<uint32_t> buf(2 * 1024);
        while (offered < total) {
            rng = rng * 1664525u + 1013904223u;
            size_t chunk = 1 + (rng >> 16) % 1000;
            for (size_t i = 0; i < chunk; i++) {
                buf[2 * i] = n + static_cast<uint32_t>(i);
                buf[2 * i + 1] = (n + static_cast<uint32_t>(i)) * 2654435761u;
            }
            size_t acc = r.write(buf.data(), chunk);
            n += static_cast<uint32_t>(acc); /* a dropped tail is not part of the stream: the sequence stays gap-free */
            offered += chunk;
        }
        done.store(true);
    });
    uint64_t read = 0, bad = 0;
    uint32_t expect = 0;
    std::vector<uint32_t> buf(2 * 777);
    for (;;) {
        size_t got = r.read(buf.data(), 777);
        for (size_t i = 0; i < got; i++) {
            if (buf[2 * i] != expect || buf[2 * i + 1] != expect * 2654435761u) bad++;
            expect++;
        }
        read += got;
        if (slow_consumer && (read / 777) % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (got == 0) {
            if (done.load() && r.available() == 0) break;
            std::this_thread::yield();
        }
    }
    prod.join();
    CHECK_EQ(bad, static_cast<uint64_t>(0));
    CHECK_EQ(read + r.frames_dropped(), offered);
    CHECK_EQ(read, r.frames_written());
    if (slow_consumer) CHECK(r.frames_dropped() > 0); /* the small ring really overflowed */
    else CHECK_EQ(r.frames_dropped(), static_cast<uint64_t>(0));
}

void test_ring() {
    basics();
    exact_fit_and_full();
    wrap();
    two_threads(20.0, false, 900000); /* the ring (960000 frames) holds the whole stream: nothing may be dropped */
    two_threads(0.05, true, 3000000);  /* 2400 frames and a slow consumer: drops, but order and content hold */
}
