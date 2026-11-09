/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "processing_sink.hpp"
#include "siggen.hpp"

#include <atomic>
#include <thread>

using namespace cassette;

namespace {

/* The reference chain, built by hand from the separate stages in the documented order. */
sig::Bytes reference(const AudioFormat &f, const sig::Bytes &in, size_t frames, bool dc, double gain_db, bool hpf,
                     bool lim) {
    const size_t ns = frames * f.channels;
    std::vector<float> w(ns);
    for (size_t i = 0; i < ns; ++i) w[i] = float(sig::get(in, i, f.bits)) / pcm::scale(f.bits);
    DcBlocker d(f.rate, f.channels);
    SoftGain g;
    g.set_db(gain_db);
    HighPass h(f.rate, f.channels);
    Limiter l(f.rate, f.channels);
    if (dc) d.process(w.data(), frames);
    g.process(w.data(), ns);
    if (hpf) h.process(w.data(), frames);
    if (lim) l.process(w.data(), frames);
    sig::Bytes out(in.size());
    for (size_t i = 0; i < ns; ++i) {
        bool c = false;
        pcm::write(out.data() + i * pcm::bytes_per_sample(f.bits), f.bits, pcm::quantize(w[i], f.bits, c));
    }
    return out;
}

void defaults() {
    AudioFormat f{48000, 1, 16};
    sig::FakeSink fake(f, 1);
    ProcessingSink p(f, &fake);
    CHECK(p.dc_blocker());
    CHECK(!p.highpass());
    CHECK(!p.limiter());
    CHECK_EQ(p.soft_gain_db(), 0.0f);
}

void order_matches_reference() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 2, bits};
        const size_t n = 9600;
        auto in = sig::sine(f, n, 440.0, {-9.0, -15.0}, 300.0 * (bits == 24 ? 256 : 1));
        struct Case { bool dc; double gain; bool hpf; bool lim; };
        for (Case c : {Case{true, 0.0, false, false}, Case{true, 13.5, false, false}, Case{true, 18.0, true, true},
                       Case{false, 24.0, false, true}, Case{false, 0.0, true, false}}) {
            sig::FakeSink fake(f, n);
            ProcessingSink p(f, &fake, 256);
            p.set_dc_blocker(c.dc);
            p.set_soft_gain_db(c.gain);
            p.set_highpass(c.hpf);
            p.set_limiter(c.lim);
            CHECK_EQ(p.write(in.data(), n), n);
            CHECK(fake.data == reference(f, in, n, c.dc, c.gain, c.hpf, c.lim));
        }
        /* order evidence: with the limiter after the gain, +24 dB on a -9 dBFS signal never saturates */
        sig::FakeSink fake(f, n);
        ProcessingSink p(f, &fake);
        p.set_soft_gain_db(24.0);
        p.set_limiter(true);
        p.write(in.data(), n);
        CHECK_EQ(p.clip_count(), 0u);
        CHECK_EQ(p.levels().ch[0].clips, 0u);
        CHECK(p.levels().ch[0].peak_db <= -0.99f);
        /* and without it, it does (counted once per saturated sample, also by the meter at full scale) */
        sig::FakeSink fake2(f, n);
        ProcessingSink p2(f, &fake2);
        p2.set_soft_gain_db(24.0);
        p2.write(in.data(), n);
        CHECK(p2.clip_count() > 0u);
        CHECK(p2.levels().ch[0].clips > 0u);
        CHECK_EQ(p2.levels().ch[0].peak_db, 0.0f);
    }
}

void any_block_size_and_counts() {
    AudioFormat f{48000, 1, 24};
    const size_t n = 6000;
    auto in = sig::sine(f, n, 1000.0, {-20.0}, 5000.0);
    const sig::Bytes in_copy = in;
    sig::FakeSink whole(f, n);
    ProcessingSink pw(f, &whole, 8192);
    CHECK_EQ(pw.write(in.data(), n), n);
    /* the same stream in odd pieces through a small chunk gives identical bytes (state carried across blocks) */
    sig::FakeSink pieces(f, n);
    ProcessingSink pp(f, &pieces, 64);
    size_t off = 0;
    const size_t sizes[] = {1, 7, 63, 64, 65, 1000, 0, 2500};
    for (size_t k : sizes) {
        CHECK_EQ(pp.write(in.data() + off * 3, k), k);
        off += k;
    }
    CHECK_EQ(pp.write(in.data() + off * 3, n - off), n - off);
    CHECK(pieces.data == whole.data);
    CHECK(in == in_copy); /* the caller's buffer is never written */
    CHECK_EQ(pp.levels().frames, uint64_t(n));

    /* returns the wrapped sink's count unchanged: a sink with room for 100 frames */
    sig::FakeSink small(f, 100);
    ProcessingSink ps(f, &small);
    CHECK_EQ(ps.write(in.data(), 300), size_t(100));
    CHECK_EQ(ps.write(in.data(), 300), size_t(0));
    CHECK_EQ(small.got, size_t(100));
    /* over several chunks it stops at the first short write and returns the sum */
    sig::FakeSink mid(f, 150);
    ProcessingSink pm(f, &mid, 64);
    CHECK_EQ(pm.write(in.data(), 300), size_t(150));
    CHECK_EQ(mid.calls, size_t(3));
    /* no wrapped sink: nothing accepted */
    ProcessingSink pn(f, nullptr);
    CHECK_EQ(pn.write(in.data(), 10), size_t(0));
    /* unsupported format passes through unchanged */
    AudioFormat f32{48000, 1, 32};
    std::vector<uint8_t> raw(40);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t(i * 7);
    sig::FakeSink fr(AudioFormat{48000, 2, 16}, 10); /* 4 bytes a frame, like the 32-bit format */
    ProcessingSink pr(f32, &fr);
    pr.set_soft_gain_db(12.0);
    CHECK_EQ(pr.write(raw.data(), 10), size_t(10));
    CHECK(fr.data == raw);
}

/* The weak headset (-29 dBFS peaks at +13 dB card level) with SOFT gain on top: assert the numbers. */
void weak_headset_with_soft_gain() {
    AudioFormat f{48000, 1, 16};
    const size_t period = 480;
    auto in = sig::sine(f, period, 1000.0, {-29.0});
    auto run = [&](double soft_db) {
        sig::FakeSink fake(f, 400 * period);
        ProcessingSink p(f, &fake);
        p.set_soft_gain_db(soft_db);
        for (int i = 0; i < 400; ++i) p.write(in.data(), period); /* 4 s */
        return p.levels().ch[0];
    };
    auto none = run(0.0);
    CHECK_NEAR(none.peak_db, -29.0, 0.1);
    CHECK(none.zone == Zone::Low);
    CHECK_EQ(none.bar, 4);
    /* 10.8 dB is set as 11.0 dB (0.5 dB steps): -29 + 11 = -18.0 dBFS, right on the bottom edge of the target zone
     * (which side of the edge a single sine lands on depends on sample rounding, so only the level and bar are asserted) */
    auto soft = run(10.8);
    CHECK_NEAR(soft.peak_db, -18.0, 0.1);
    CHECK_EQ(soft.bar, 8);
    /* one step lower stays under the zone, one step higher is in it */
    auto under = run(10.5);
    CHECK_NEAR(under.peak_db, -18.5, 0.1);
    CHECK(under.zone == Zone::Low);
    auto over = run(11.5);
    CHECK_NEAR(over.peak_db, -17.5, 0.1);
    CHECK(over.zone == Zone::InZone);
    /* +18 dB lands at -11 dBFS, mid zone; +24 dB at -5 dBFS reads High */
    CHECK_NEAR(run(18.0).peak_db, -11.0, 0.1);
    CHECK(run(18.0).zone == Zone::InZone);
    CHECK(run(24.0).zone == Zone::High);
}

/* settings change from a UI thread while the capture thread writes: atomics, no lock, no crash, no race */
void runtime_settings() {
    AudioFormat f{48000, 2, 16};
    const size_t period = 256;
    auto in = sig::sine(f, period, 1000.0, {-12.0});
    sig::FakeSink fake(f, 4000 * period);
    ProcessingSink p(f, &fake, 128);
    std::atomic<bool> done{false};
    std::thread ui([&] {
        int i = 0;
        while (!done.load()) {
            p.set_soft_gain_db((i % 49) * 0.5);
            p.set_highpass(i % 3 == 0);
            p.set_limiter(i % 5 == 0);
            p.set_dc_blocker(i % 7 != 0);
            (void)p.levels();
            ++i;
        }
    });
    size_t got = 0;
    for (int i = 0; i < 4000; ++i) got += p.write(in.data(), period);
    done.store(true);
    ui.join();
    CHECK_EQ(got, 4000 * period);
    CHECK_EQ(p.levels().frames, uint64_t(4000 * period));
}

void no_alloc_after_construction() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 2, bits};
        auto in = sig::sine(f, 5000, 1000.0, {-6.0});
        sig::FakeSink fake(f, 20 * 5000); /* reserves its buffer up front */
        ProcessingSink p(f, &fake, 512);
        p.set_soft_gain_db(24.0);
        p.set_highpass(true);
        p.set_limiter(true);
        const long long before = alloc_count();
        for (int i = 0; i < 10; ++i) p.write(in.data(), 5000);
        for (int i = 0; i < 10; ++i) p.write(in.data(), size_t(i * 37 + 1));
        (void)p.levels();
        CHECK_EQ(alloc_count() - before, 0LL);
    }
}

/* The live monitor's tap: the same processed frames as the recording gets (after the DC blocker, SOFT gain, filters), in
 * the same blocks; its result is ignored (a tap that accepts nothing changes nothing in the recording); no allocation. */
void tap_sees_what_is_recorded() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 2, bits};
        const size_t n = 6000;
        auto in = sig::sine(f, n, 440.0, {-9.0, -15.0}, 300.0 * (bits == 24 ? 256 : 1));
        sig::FakeSink fake(f, n), tapped(f, n), refuse(f, 0);
        ProcessingSink p(f, &fake, 512);
        p.set_soft_gain_db(9.0);
        p.set_highpass(true);
        p.set_tap(&tapped);
        CHECK_EQ(p.write(in.data(), n), n);
        CHECK(tapped.data == fake.data); /* byte for byte what the writer got */
        CHECK_EQ(tapped.got, n);
        /* a tap that takes nothing: the recording is whole */
        sig::FakeSink fake2(f, n);
        ProcessingSink q(f, &fake2, 512);
        q.set_soft_gain_db(9.0);
        q.set_highpass(true);
        q.set_tap(&refuse);
        CHECK_EQ(q.write(in.data(), n), n);
        CHECK(fake2.data == fake.data);
        /* removed: no more frames */
        p.set_tap(nullptr);
        const size_t before = tapped.got;
        sig::FakeSink fake3(f, n);
        ProcessingSink r(f, &fake3, 512);
        r.set_tap(&tapped);
        r.set_tap(nullptr);
        r.write(in.data(), 1000);
        CHECK_EQ(tapped.got, before);
        /* no allocation with a tap */
        sig::FakeSink fake4(f, 20 * 5000), tap4(f, 20 * 5000);
        ProcessingSink t(f, &fake4, 512);
        t.set_tap(&tap4);
        const long long a0 = alloc_count();
        for (int i = 0; i < 10; ++i) t.write(in.data(), 5000);
        CHECK_EQ(alloc_count() - a0, 0LL);
    }
}

}  // namespace

void test_sink() {
    defaults();
    order_matches_reference();
    any_block_size_and_counts();
    weak_headset_with_soft_gain();
    runtime_settings();
    no_alloc_after_construction();
    tap_sees_what_is_recorded();
}
