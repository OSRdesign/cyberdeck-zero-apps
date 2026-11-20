/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's DSP bench (see dsp_bench.hpp).
 */
#include "dsp_bench.hpp"

#include "engine.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace vinyl {

namespace {

double cpu_now() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

/* a block of 16-bit stereo: a sine and a little noise (so no filter state ever settles to zero) */
void make_block(std::vector<int16_t> &b, size_t frames, uint32_t &seed, double &ph) {
    b.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.1;
        const double v = 0.6 * std::sin(ph) + noise;
        ph += 2 * 3.14159265358979 * 440.0 / 44100.0;
        b[2 * f] = static_cast<int16_t>(v * 32767);
        b[2 * f + 1] = static_cast<int16_t>(-v * 32767);
    }
}

}  // namespace

DspBench run_dsp_bench(double seconds) {
    constexpr size_t kBlock = 2048;
    constexpr uint32_t kRate = 44100;
    const size_t blocks = static_cast<size_t>(std::max(1.0, seconds * kRate / kBlock));
    std::vector<int16_t> a, b;
    std::vector<float> fa(kBlock * 2), fb(kBlock * 2), mix(kBlock * 2);
    std::vector<int16_t> out(kBlock * 2);
    uint32_t seed = 12345;
    double ph = 0;
    make_block(a, kBlock, seed, ph);
    make_block(b, kBlock, seed, ph);
    Limiter lim(kRate);
    Equalizer eq;
    eq.configure(EqGains{4, 3, 2, 1, -1, -1, 1, 2, 3, 4}, kRate); /* every band at work */
    volatile int64_t sink = 0;
    auto chain = [&](bool with_eq, bool with_fade, size_t i) {
        pcm_to_stereo_float(a.data(), 16, 2, kBlock, 0.9f, fa.data());
        float *st = fa.data();
        if (with_fade) {
            pcm_to_stereo_float(b.data(), 16, 2, kBlock, 0.9f, fb.data());
            crossfade_mix(fa.data(), fb.data(), mix.data(), kBlock, (i * kBlock) % (6 * kRate), 6 * kRate);
            st = mix.data();
        }
        if (with_eq) eq.process(st, kBlock);
        for (size_t k = 0; k < 2 * kBlock; ++k) st[k] *= 0.95f;
        lim.process(st, kBlock);
        stereo_float_to_pcm(st, kBlock, 2, SampleFormat::S16_LE, out.data());
        sink = sink + out[i % (2 * kBlock)];
    };
    auto time_of = [&](bool with_eq, bool with_fade) {
        const double t0 = cpu_now();
        for (size_t i = 0; i < blocks; ++i) chain(with_eq, with_fade, i);
        return cpu_now() - t0;
    };
    const double audio = static_cast<double>(blocks * kBlock) / kRate;
    const double t_base = time_of(false, false);
    const double t_eq = time_of(true, false);
    const double t_fade = time_of(false, true);
    const double t_all = time_of(true, true);
    DspBench r;
    r.seconds = audio;
    r.base_pct = 100.0 * t_base / audio;
    r.eq_pct = 100.0 * std::max(0.0, t_eq - t_base) / audio;
    r.fade_pct = 100.0 * std::max(0.0, t_fade - t_base) / audio;
    r.total_pct = 100.0 * t_all / audio;
    (void)sink;
    return r;
}

std::string dsp_bench_text(const DspBench &b) {
    char t[512];
    std::snprintf(t, sizeof t,
                  "vinyl dsp bench: %.0f s of 44.1 kHz stereo, %% of one core\n"
                  "  float path (conversion, gain, limiter): %.2f %%\n"
                  "  equalizer, 10 bands at work:            %.2f %%\n"
                  "  crossfade mix (second track):           %.2f %%\n"
                  "  all together (a fade with the EQ on):   %.2f %%\n"
                  "  (each decoder adds its own: report 059, FLAC 0.65, MP3 0.8, Vorbis 2.1, Opus 2.6-3.6 %% on a Zero 2 W)\n",
                  b.seconds, b.base_pct, b.eq_pct, b.fade_pct, b.total_pct);
    return t;
}

}  // namespace vinyl
