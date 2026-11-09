/* SPDX-License-Identifier: MIT
 *
 * Cassette DSP tests at the rates a card really records at (16000 Hz speech headsets, 22050, 96000): the DC blocker, the
 * 80 Hz high-pass, the limiter, the meter's hold and zone timing and the whole processing chain take the rate, never 48000.
 */
#include "check.hpp"
#include "dc_blocker.hpp"
#include "highpass.hpp"
#include "limiter.hpp"
#include "meter.hpp"
#include "processing_sink.hpp"
#include "siggen.hpp"

#include <vector>

using namespace cassette;

namespace {

std::vector<float> tone(uint32_t rate, size_t frames, double freq, double amp, double offset = 0.0) {
    std::vector<float> v(frames);
    for (size_t i = 0; i < frames; ++i) v[i] = float(offset + amp * std::sin(2.0 * sig::kPi * freq * double(i) / double(rate)));
    return v;
}

double tail_rms_db(const std::vector<float> &v, size_t frames) {
    double s = 0;
    for (size_t i = v.size() - frames; i < v.size(); ++i) s += double(v[i]) * v[i];
    return 10.0 * std::log10(s / double(frames));
}

double peak_from(const std::vector<float> &v, size_t from) {
    double m = 0;
    for (size_t i = from; i < v.size(); ++i) m = std::max(m, double(std::fabs(v[i])));
    return m;
}

const uint32_t kRates[] = {16000, 22050, 96000};

void dc_blocker_follows_the_rate() {
    for (uint32_t rate : kRates) {
        DcBlocker d(rate, 1);
        CHECK_NEAR(d.coefficient(), 1.0 - 2.0 * sig::kPi * 3.5 / double(rate), 1e-9);
        /* the same 45 ms time constant in seconds: a constant offset is gone after half a second at any rate */
        std::vector<float> dc(rate, 0.2f);
        d.process(dc.data(), dc.size());
        CHECK(peak_from(dc, rate / 2) < 1e-4);
        /* a 1 kHz sine passes within 0.05 dB */
        DcBlocker d2(rate, 1);
        auto ref = tone(rate, rate, 1000.0, 0.5);
        auto v = tone(rate, rate, 1000.0, 0.5, 0.1);
        d2.process(v.data(), v.size());
        CHECK_NEAR(tail_rms_db(v, rate / 2), tail_rms_db(ref, rate / 2), 0.05);
    }
}

void highpass_corner_is_80_hz() {
    for (uint32_t rate : kRates) {
        auto gain_db = [rate](double freq) {
            HighPass h(rate, 1);
            auto ref = tone(rate, rate * 2, freq, 0.5);
            auto v = ref;
            h.process(v.data(), v.size());
            return tail_rms_db(v, rate) - tail_rms_db(ref, rate);
        };
        CHECK_NEAR(gain_db(1000.0), 0.0, 0.1);
        CHECK_NEAR(gain_db(80.0), -3.01, 0.15);
        CHECK(gain_db(20.0) < -20.0);
        CHECK(gain_db(300.0) > -0.3);
    }
}

void limiter_release_is_100_ms() {
    for (uint32_t rate : kRates) {
        Limiter l(rate, 1);
        auto burst = tone(rate, rate / 10, 1000.0, 4.0);
        l.process(burst.data(), burst.size());
        CHECK(peak_from(burst, 0) <= l.ceiling());
        CHECK(l.gain_reduction_db() > 5.0f);
        /* back within 0.1 dB after one second, and still clearly reduced after 20 ms (100 ms release time) */
        Limiter r(rate, 1);
        r.process(burst.data(), burst.size());
        Limiter r2(rate, 1);
        auto b2 = tone(rate, rate / 10, 1000.0, 4.0);
        r2.process(b2.data(), b2.size());
        auto q = tone(rate, rate / 50, 1000.0, 0.01);
        r2.process(q.data(), q.size());
        CHECK(r2.gain_reduction_db() > 3.0f);
        auto after = tone(rate, rate, 1000.0, 0.1);
        r.process(after.data(), after.size());
        CHECK(r.gain_reduction_db() < 0.1f);
    }
}

void meter_timing_is_in_seconds() {
    for (uint32_t rate : kRates) {
        const AudioFormat f{rate, 1, 16};
        const size_t period = rate / 100; /* 10 ms */
        Meter a(f);
        auto loud = sig::sine(f, period, 1000.0, {-6.0});
        auto quiet = sig::sine(f, period, 1000.0, {-30.0});
        CHECK_NEAR(Meter(f).snapshot().ch[0].hold_db, -120.0, 0.001);
        a.feed(loud.data(), period);
        for (int i = 0; i < 149; ++i) a.feed(quiet.data(), period); /* 1.49 s of audio, whatever the rate */
        CHECK_NEAR(a.snapshot().ch[0].hold_db, -6.0, 0.1);          /* still held */
        for (int i = 0; i < 50; ++i) a.feed(quiet.data(), period);  /* +0.5 s: falling at 20 dB/s */
        CHECK_NEAR(a.snapshot().ch[0].hold_db, -15.8, 0.15);
        CHECK_EQ(a.snapshot().frames, uint64_t(200 * period));
        /* the dB values do not depend on the rate: a -20 dBFS sine reads -20 peak and -23.01 RMS */
        Meter m(f);
        auto s = sig::sine(f, rate / 10, 1000.0, {-20.0});
        m.feed(s.data(), rate / 10);
        CHECK_NEAR(m.snapshot().ch[0].peak_db, -20.0, 0.05);
        CHECK_NEAR(m.snapshot().ch[0].rms_db, -23.01, 0.05);
        /* the 3 s "too low" window in audio time: quiet for 2.9 s is still InZone after a good peak, then Low */
        Meter z(f);
        auto good = sig::sine(f, period, 1000.0, {-12.0});
        z.feed(good.data(), period);
        for (int i = 0; i < 289; ++i) z.feed(quiet.data(), period);
        CHECK(z.snapshot().ch[0].zone == Zone::InZone);
        for (int i = 0; i < 20; ++i) z.feed(quiet.data(), period);
        CHECK(z.snapshot().ch[0].zone == Zone::Low);
    }
}

void chain_at_16000() {
    const AudioFormat f{16000, 1, 16};
    sig::FakeSink out(f, 100000);
    ProcessingSink p(f, &out);
    p.set_highpass(true);
    p.set_limiter(true);
    auto in = sig::sine(f, 16000, 1000.0, {-12.0});
    CHECK_EQ(p.write(in.data(), 16000), size_t(16000));
    CHECK_EQ(out.got, size_t(16000));
    /* a 1 kHz tone is untouched by the 80 Hz high-pass and stays under the limiter ceiling: -12 dBFS peak */
    Meter m(f);
    m.feed(out.data.data() + 2 * 8000, 8000);
    CHECK_NEAR(m.snapshot().ch[0].peak_db, -12.0, 0.2);
    CHECK_EQ(p.levels().frames, uint64_t(16000));
    /* a 30 Hz rumble is cut by the high-pass at this rate */
    sig::FakeSink out2(f, 100000);
    ProcessingSink q(f, &out2);
    q.set_highpass(true);
    auto rumble = sig::sine(f, 32000, 30.0, {-12.0});
    q.write(rumble.data(), 32000);
    Meter m2(f);
    m2.feed(out2.data.data() + 2 * 16000, 16000);
    CHECK(m2.snapshot().ch[0].peak_db < -22.0);
}

}  // namespace

void test_rates() {
    dc_blocker_follows_the_rate();
    highpass_corner_is_80_hz();
    limiter_release_is_100_ms();
    meter_timing_is_in_seconds();
    chain_at_16000();
}
