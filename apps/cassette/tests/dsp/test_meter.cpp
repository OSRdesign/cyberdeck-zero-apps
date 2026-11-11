/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "meter.hpp"
#include "siggen.hpp"

#include <atomic>
#include <thread>

using namespace cassette;

namespace {

constexpr size_t kPeriod = 480; /* 10 ms at 48 kHz, 10 whole 1 kHz cycles */

void dbfs_accuracy() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        for (double lv : {0.0, -1.0, -6.0, -12.0, -18.0, -20.0, -29.0, -40.0, -60.0}) {
            AudioFormat f{48000, 1, bits};
            Meter m(f);
            auto b = sig::sine(f, kPeriod, 1000.0, {lv});
            m.feed(b.data(), kPeriod);
            auto s = m.snapshot();
            CHECK_EQ(s.channels, 1);
            CHECK_NEAR(s.ch[0].peak_db, lv, 0.1);
            CHECK_NEAR(s.ch[0].rms_db, lv - 3.0103, 0.1); /* plain RMS: a -20 dBFS sine reads -23.01 */
        }
    }
    /* the study's reference point, exact to 0.01 dB in 24 bits */
    AudioFormat f{48000, 1, 24};
    Meter m(f);
    auto b = sig::sine(f, kPeriod, 1000.0, {-20.0});
    m.feed(b.data(), kPeriod);
    CHECK_NEAR(m.snapshot().ch[0].peak_db, -20.0, 0.01);
    CHECK_NEAR(m.snapshot().ch[0].rms_db, -23.01, 0.01);
}

void stereo_and_silence() {
    AudioFormat f{48000, 2, 16};
    Meter m(f);
    auto b = sig::sine(f, kPeriod, 1000.0, {-12.0, -300.0});
    m.feed(b.data(), kPeriod);
    auto s = m.snapshot();
    CHECK_EQ(s.channels, 2);
    CHECK_NEAR(s.ch[0].peak_db, -12.0, 0.1);
    CHECK_EQ(s.ch[1].peak_db, Meter::kFloorDb); /* silence reads the -120 dBFS floor, never -inf */
    CHECK_EQ(s.ch[1].rms_db, Meter::kFloorDb);
    CHECK_EQ(s.ch[1].bar, 0);
    CHECK(s.ch[1].zone == Zone::Low);
    CHECK_EQ(Meter::to_db(0.0), -120.0f);
    CHECK_EQ(Meter::to_db(1e-9), -120.0f);
    /* before the first period */
    Meter m2(f);
    CHECK_EQ(m2.snapshot().channels, 0);
    /* more than two channels: the first two are metered */
    AudioFormat f4{48000, 4, 16};
    Meter m4(f4);
    auto b4 = sig::sine(f4, kPeriod, 1000.0, {-6.0, -12.0, 0.0, 0.0});
    m4.feed(b4.data(), kPeriod);
    CHECK_EQ(m4.snapshot().channels, 2);
    CHECK_NEAR(m4.snapshot().ch[1].peak_db, -12.0, 0.1);
    CHECK_EQ(m4.snapshot().ch[0].clips, 0u);
    /* unsupported format: ignored */
    AudioFormat f32{48000, 1, 32};
    Meter m32(f32);
    std::vector<uint8_t> z(kPeriod * 4, 0x7f);
    m32.feed(z.data(), kPeriod);
    CHECK_EQ(m32.snapshot().periods, 0u);
}

void bar_steps() {
    CHECK_EQ(Meter::bar_steps(-120.0f), 0);
    CHECK_EQ(Meter::bar_steps(-40.01f), 0);
    CHECK_EQ(Meter::bar_steps(-40.0f), 1);
    for (int i = 0; i < 15; ++i) {
        const float th = -40.0f + float(i) * (40.0f / 14.0f);
        CHECK_EQ(Meter::bar_steps(th), i + 1);
        CHECK_EQ(Meter::bar_steps(th - 0.01f), i);
    }
    CHECK_EQ(Meter::bar_steps(0.0f), 15);
    CHECK_EQ(Meter::bar_steps(-29.0f), 4);  /* the weak headset */
    CHECK_EQ(Meter::bar_steps(-18.0f), 8);  /* bottom of the target zone */
    CHECK_EQ(Meter::bar_steps(-6.0f), 12);  /* top of the target zone */
    /* full-scale samples of either sign light the top step */
    AudioFormat f{48000, 1, 16};
    Meter m(f);
    auto b = sig::square(f, kPeriod, 32767);
    m.feed(b.data(), kPeriod);
    CHECK_EQ(m.snapshot().ch[0].bar, 15);
    CHECK_EQ(m.snapshot().ch[0].peak_db, 0.0f);
}

void clip_counter_and_lamp() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 1, bits};
        Meter m(f);
        sig::Bytes b(kPeriod * pcm::bytes_per_sample(bits));
        sig::put(b, 3, bits, pcm::max_pos(bits));
        sig::put(b, 7, bits, pcm::min_neg(bits));
        sig::put(b, 9, bits, pcm::max_pos(bits) - 1); /* not full scale */
        m.feed(b.data(), kPeriod, 0);
        CHECK_EQ(m.snapshot().ch[0].clips, 2u);
        m.feed(b.data(), kPeriod, 10000);
        CHECK_EQ(m.snapshot().ch[0].clips, 4u);
        m.reset();
        CHECK_EQ(m.snapshot().ch[0].clips, 0u);
    }
    /* PEAK lamp: -0.5 dBFS lights it, held 1 s */
    AudioFormat f{48000, 1, 16};
    Meter m(f);
    auto hot = sig::sine(f, kPeriod, 1000.0, {-0.5});
    auto quiet = sig::sine(f, kPeriod, 1000.0, {-30.0});
    m.feed(hot.data(), kPeriod, 0);
    CHECK(m.snapshot().ch[0].peak_lamp);
    CHECK_EQ(m.snapshot().ch[0].clips, 0u);
    m.feed(quiet.data(), kPeriod, 900000);
    CHECK(m.snapshot().ch[0].peak_lamp);
    m.feed(quiet.data(), kPeriod, 1000000);
    CHECK(m.snapshot().ch[0].peak_lamp);
    m.feed(quiet.data(), kPeriod, 1010000);
    CHECK(!m.snapshot().ch[0].peak_lamp);
    /* -1.2 dBFS never lights it; exactly -1 dBFS does */
    Meter m2(f);
    auto warm = sig::sine(f, kPeriod, 1000.0, {-1.2});
    m2.feed(warm.data(), kPeriod, 0);
    CHECK(!m2.snapshot().ch[0].peak_lamp);
    sig::Bytes one(2 * kPeriod);
    sig::put(one, 5, 16, std::ceil(32767.0 * std::pow(10.0, -1.0 / 20.0)));
    m2.feed(one.data(), kPeriod, 10000);
    CHECK(m2.snapshot().ch[0].peak_lamp);
    CHECK(m2.snapshot().ch[0].peak_db >= -1.0f);
}

void peak_hold_fake_clock() {
    AudioFormat f{48000, 1, 16};
    Meter m(f);
    auto loud = sig::sine(f, kPeriod, 1000.0, {-6.0});
    auto quiet = sig::sine(f, kPeriod, 1000.0, {-30.0});
    m.feed(loud.data(), kPeriod, 0);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -6.0, 0.1);
    uint64_t t = 0;
    auto run_to = [&](uint64_t until) {
        while (t < until) {
            t += 10000;
            m.feed(quiet.data(), kPeriod, t);
        }
    };
    run_to(1400000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -6.0, 0.1);
    CHECK_NEAR(m.snapshot().ch[0].peak_db, -30.0, 0.1);
    run_to(1500000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -6.0, 0.1); /* still held at exactly 1.5 s */
    run_to(2000000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -16.0, 0.1); /* 0.5 s of falling at 20 dB/s */
    CHECK_EQ(m.snapshot().ch[0].hold_bar, Meter::bar_steps(m.snapshot().ch[0].hold_db));
    run_to(3000000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -30.0, 0.1); /* met the current peak and follows it */
    /* a new higher peak restarts the hold */
    m.feed(loud.data(), kPeriod, t += 10000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -6.0, 0.1);
    /* the audio clock (frames / rate) drives it when no clock is given */
    Meter a(f);
    a.feed(loud.data(), kPeriod);
    for (int i = 0; i < 149; ++i) a.feed(quiet.data(), kPeriod); /* 1.5 s */
    CHECK_NEAR(a.snapshot().ch[0].hold_db, -6.0, 0.1);
    for (int i = 0; i < 50; ++i) a.feed(quiet.data(), kPeriod); /* +0.5 s */
    CHECK_NEAR(a.snapshot().ch[0].hold_db, -15.8, 0.1); /* held from t = 10 ms: 0.49 s of falling */
    CHECK_EQ(a.snapshot().frames, uint64_t(200 * kPeriod));
    CHECK_EQ(a.snapshot().periods, 200u);
}

void target_zone() {
    AudioFormat f{48000, 1, 16};
    auto zone_after = [&](double lv, uint64_t us) {
        Meter m(f);
        auto b = sig::sine(f, kPeriod, 1000.0, {lv});
        for (uint64_t t = 0; t <= us; t += 10000) m.feed(b.data(), kPeriod, t);
        return m.snapshot().ch[0];
    };
    /* the owner's headset at +13 dB: speech peaks near -29 dBFS */
    auto weak = zone_after(-29.0, 4000000);
    CHECK_NEAR(weak.peak_db, -29.0, 0.1);
    CHECK(weak.zone == Zone::Low);
    CHECK_EQ(weak.bar, 4);
    CHECK(zone_after(-12.0, 4000000).zone == Zone::InZone);
    CHECK(zone_after(-17.9, 4000000).zone == Zone::InZone);
    CHECK(zone_after(-18.3, 4000000).zone == Zone::Low);
    CHECK(zone_after(-6.0, 4000000).zone == Zone::InZone);
    CHECK(zone_after(-5.5, 4000000).zone == Zone::High);

    /* transitions on a fake clock: a -3 dBFS burst, then a -12 dBFS phrase, then -40 dBFS room noise */
    Meter m(f);
    auto hot = sig::sine(f, kPeriod, 1000.0, {-3.0});
    auto mid = sig::sine(f, kPeriod, 1000.0, {-12.0});
    auto low = sig::sine(f, kPeriod, 1000.0, {-40.0});
    m.feed(low.data(), kPeriod, 0);
    CHECK(m.snapshot().ch[0].zone == Zone::Low);
    m.feed(hot.data(), kPeriod, 100000);
    CHECK(m.snapshot().ch[0].zone == Zone::High);
    m.feed(mid.data(), kPeriod, 1500000);
    CHECK(m.snapshot().ch[0].zone == Zone::High);   /* High holds 1.5 s */
    m.feed(mid.data(), kPeriod, 1610000);
    CHECK(m.snapshot().ch[0].zone == Zone::InZone);
    m.feed(low.data(), kPeriod, 3000000);
    CHECK(m.snapshot().ch[0].zone == Zone::InZone); /* reached -18 within the last 3 s */
    m.feed(low.data(), kPeriod, 4610000);
    CHECK(m.snapshot().ch[0].zone == Zone::InZone); /* exactly 3 s after the last -12 dBFS period */
    m.feed(low.data(), kPeriod, 4620000);
    CHECK(m.snapshot().ch[0].zone == Zone::Low);
}

/* The UI thread reads snapshots while the capture thread publishes: every snapshot must be internally consistent.
 * The writer feeds stereo squares of one amplitude per period, so peak == rms and ch0 == ch1 in every true state. */
void snapshot_two_threads() {
    AudioFormat f{48000, 2, 16};
    Meter m(f);
    constexpr int kBlocks = 64;
    std::vector<sig::Bytes> blocks;
    for (int i = 0; i < kBlocks; ++i) blocks.push_back(sig::square(f, 96, 100 + i * 500));
    std::atomic<bool> done{false};
    long reads = 0, bad = 0, nonzero = 0;
    std::thread reader([&] {
        uint64_t last_periods = 0;
        while (!done.load(std::memory_order_acquire)) {
            auto s = m.snapshot();
            ++reads;
            if (s.periods == 0) continue;
            ++nonzero;
            bool ok = s.channels == 2 && s.frames == s.periods * 96 && s.periods >= last_periods &&
                      s.ch[0].peak_db == s.ch[1].peak_db && s.ch[0].rms_db == s.ch[0].peak_db &&
                      s.ch[1].rms_db == s.ch[1].peak_db && s.ch[0].bar == Meter::bar_steps(s.ch[0].peak_db);
            last_periods = s.periods;
            if (!ok) ++bad;
        }
    });
    for (int i = 0; i < 200000; ++i) m.feed(blocks[i % kBlocks].data(), 96);
    done.store(true, std::memory_order_release);
    reader.join();
    CHECK(reads > 0);
    CHECK(nonzero > 0);
    CHECK_EQ(bad, 0L);
    CHECK_EQ(m.snapshot().periods, 200000u);
}

void no_alloc() {
    AudioFormat f{48000, 2, 24};
    Meter m(f);
    auto b = sig::sine(f, kPeriod, 1000.0, {-12.0, -20.0});
    const long long before = alloc_count();
    for (int i = 0; i < 100; ++i) {
        m.feed(b.data(), kPeriod);
        (void)m.snapshot();
    }
    CHECK_EQ(alloc_count() - before, 0LL);
}

/* feed_peaks (the playback meter): the same bar, hold and zone rules as feed() from a level per channel; no lamp, no clips;
 * a level at the same dBFS as a fed sine gives the same bar and hold; no allocation. */
void feed_peaks_rules() {
    AudioFormat f{48000, 2, 16};
    Meter m(f), ref(f);
    auto b = sig::sine(f, kPeriod, 1000.0, {-12.0, -12.0});
    ref.feed(b.data(), kPeriod, 1000);
    const float lv[2] = {-12.0f, -30.0f};
    m.feed_peaks(lv, 2, 1000);
    MeterSnapshot s = m.snapshot(), r = ref.snapshot();
    CHECK_EQ(s.channels, 2);
    CHECK_EQ(s.ch[0].bar, r.ch[0].bar);
    CHECK_EQ(s.ch[0].hold_bar, r.ch[0].hold_bar);
    CHECK_EQ(s.ch[0].bar, Meter::bar_steps(-12.0f));
    CHECK_EQ(s.ch[1].bar, Meter::bar_steps(-30.0f));
    CHECK(s.ch[0].zone == Zone::InZone);
    CHECK(s.ch[1].zone == Zone::Low);
    /* full scale: no lamp, no clips (the PEAK lamp is the recording's clip warning) */
    const float hot[2] = {0.0f, 3.0f};
    m.feed_peaks(hot, 2, 2000);
    s = m.snapshot();
    CHECK(!s.ch[0].peak_lamp && !s.ch[1].peak_lamp);
    CHECK_EQ(s.ch[0].clips, 0u);
    CHECK_NEAR(s.ch[1].peak_db, 0.0, 1e-6); /* clamped to 0 dBFS */
    CHECK(s.ch[0].zone == Zone::High);
    /* silence: the bar drops, the hold stays 1.5 s, then falls at 20 dB/s */
    const float quiet[2] = {-120.0f, -120.0f};
    m.feed_peaks(quiet, 2, 1000000);
    s = m.snapshot();
    CHECK_EQ(s.ch[0].bar, 0);
    CHECK_NEAR(s.ch[0].hold_db, 0.0, 1e-6);
    m.feed_peaks(quiet, 2, 2000 + 1500000 + 500000);
    CHECK_NEAR(m.snapshot().ch[0].hold_db, -10.0, 0.01);
    /* mono: one channel; 0 channels or a null pointer are ignored */
    Meter mono(f);
    const float one[1] = {-6.5f};
    mono.feed_peaks(one, 1, 10);
    CHECK_EQ(mono.snapshot().channels, 1);
    mono.feed_peaks(one, 0, 20);
    mono.feed_peaks(nullptr, 1, 30);
    CHECK_EQ(mono.snapshot().periods, 1u);
    const long long before = alloc_count();
    for (int i = 0; i < 100; ++i) {
        m.feed_peaks(lv, 2, 3000000 + uint64_t(i) * 20000);
        (void)m.snapshot();
    }
    CHECK_EQ(alloc_count() - before, 0LL);
}

}  // namespace

void test_meter() {
    feed_peaks_rules();
    dbfs_accuracy();
    stereo_and_silence();
    bar_steps();
    clip_counter_and_lamp();
    peak_hold_fake_clock();
    target_zone();
    snapshot_two_threads();
    no_alloc();
}
