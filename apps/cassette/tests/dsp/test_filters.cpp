/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "dc_blocker.hpp"
#include "gain.hpp"
#include "highpass.hpp"
#include "limiter.hpp"
#include "siggen.hpp"

#include <vector>

using namespace cassette;

namespace {

std::vector<float> tone(size_t frames, uint16_t ch, double freq, double amp, double offset = 0.0) {
    std::vector<float> v(frames * ch);
    for (size_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < ch; ++c)
            v[i * ch + c] = float(offset + amp * std::sin(2.0 * sig::kPi * freq * double(i) / 48000.0));
    return v;
}

/* RMS in dB over the last `frames` frames of channel c */
double tail_rms_db(const std::vector<float> &v, uint16_t ch, uint16_t c, size_t frames) {
    const size_t n = v.size() / ch;
    double s = 0;
    for (size_t i = n - frames; i < n; ++i) s += double(v[i * ch + c]) * v[i * ch + c];
    return 10.0 * std::log10(s / double(frames));
}

double tail_peak(const std::vector<float> &v, size_t from) {
    double m = 0;
    for (size_t i = from; i < v.size(); ++i) m = std::max(m, double(std::fabs(v[i])));
    return m;
}

void dc_blocker() {
    DcBlocker d(48000, 1);
    CHECK_NEAR(d.coefficient(), 0.99954, 0.00001);
    /* a constant offset is removed (time constant 45 ms) */
    std::vector<float> dc(48000, 0.2f);
    d.process(dc.data(), dc.size());
    CHECK(tail_peak(dc, 24000) < 1e-5);
    /* a 1 kHz sine stays within 0.05 dB (stereo, with an offset on top) */
    DcBlocker d2(48000, 2);
    auto ref = tone(48000, 2, 1000.0, 0.5);
    auto v = tone(48000, 2, 1000.0, 0.5, 0.1);
    d2.process(v.data(), 48000);
    for (uint16_t c = 0; c < 2; ++c) CHECK_NEAR(tail_rms_db(v, 2, c, 24000), tail_rms_db(ref, 2, c, 24000), 0.05);
    CHECK_NEAR(tail_peak(v, 48000), 0.5, 0.5 * (std::pow(10.0, 0.05 / 20.0) - 1.0));
    /* 100 Hz speech fundamentals too */
    DcBlocker d3(48000, 1);
    auto r100 = tone(48000, 1, 100.0, 0.5);
    auto v100 = r100;
    d3.process(v100.data(), 48000);
    CHECK_NEAR(tail_rms_db(v100, 1, 0, 24000), tail_rms_db(r100, 1, 0, 24000), 0.05);
}

void highpass() {
    auto gain_db = [](double freq) {
        HighPass h(48000, 1);
        auto ref = tone(96000, 1, freq, 0.5);
        auto v = ref;
        h.process(v.data(), 96000);
        return tail_rms_db(v, 1, 0, 48000) - tail_rms_db(ref, 1, 0, 48000);
    };
    CHECK_NEAR(gain_db(1000.0), 0.0, 0.05);
    CHECK_NEAR(gain_db(80.0), -3.01, 0.1);
    CHECK(gain_db(20.0) < -20.0); /* two octaves below: about -24 dB */
    CHECK(gain_db(300.0) > -0.2);
}

void limiter() {
    /* a loud -3 dBFS sine boosted +24 dB: the limiter keeps it under the -1 dBFS ceiling at every sample */
    for (double freq : {50.0, 1000.0, 7000.0}) {
        auto v = tone(48000, 2, freq, std::pow(10.0, -3.0 / 20.0));
        SoftGain g;
        g.set_db(24.0);
        g.process(v.data(), v.size());
        Limiter l(48000, 2);
        l.process(v.data(), 48000);
        CHECK(tail_peak(v, 0) <= l.ceiling());
        CHECK(l.ceiling() < 1.0f);
        CHECK(l.gain_reduction_db() > 15.0f); /* 22 dB at the crests; the run ends near a zero crossing */
    }
    /* a single huge spike is caught on its first sample (no look-ahead, instant attack) */
    std::vector<float> spike(100, 0.0f);
    spike[50] = 40.0f;
    Limiter l(48000, 1);
    l.process(spike.data(), 100);
    CHECK(spike[50] <= l.ceiling());
    CHECK_NEAR(spike[50], l.ceiling(), 1e-6);
    /* under the ceiling and once recovered: unchanged */
    Limiter q(48000, 1);
    auto quiet = tone(4800, 1, 1000.0, 0.1);
    auto copy = quiet;
    q.process(copy.data(), 4800);
    CHECK(copy == quiet);
    /* recovery after a burst: 100 ms release, back within 0.1 dB after 1 s */
    Limiter r(48000, 1);
    auto burst = tone(4800, 1, 1000.0, 4.0);
    r.process(burst.data(), 4800);
    auto after = tone(48000, 1, 1000.0, 0.1);
    r.process(after.data(), 48000);
    CHECK(r.gain_reduction_db() < 0.1f);
}

}  // namespace

void test_filters() {
    dc_blocker();
    highpass();
    limiter();
}
