/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: deterministic synthetic signals for the encoders and the FLAC ratio measurements. Nothing here
 * is a recording.
 *   speech: a glottal pulse train (100-220 Hz, gliding) through three formant resonators that move every syllable
 *           (about 4 per second), syllables shaped by an envelope, short pauses between words and longer ones between
 *           phrases, some breath noise; peaks around -12 dBFS
 *   music:  a chord progression of 4-5 voices with harmonics (decaying partials, slight detune), a bass line and a
 *           noise-like hi-hat; stereo by panning each voice; peaks around -6 dBFS
 *   both get a white noise floor at `noise_dbfs` (a USB headset sits around -70 dBFS: P0 measured -65 to -71)
 * Output: interleaved little-endian S16 or packed S24, the capture format.
 */
#pragma once

#include "frame_sink.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace sig {

using Bytes = std::vector<uint8_t>;
constexpr double kPi = 3.14159265358979323846;

/* xorshift: same output on every machine */
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); } /* 0..1 */
    double gauss() {
        double u = uniform(), v = uniform();
        if (u < 1e-12) u = 1e-12;
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    }
};

inline void put(Bytes &b, size_t idx, uint16_t bits, double v) { /* v in -1..1 */
    const double full = bits == 24 ? 8388607.0 : 32767.0;
    long r = std::lround(v * full);
    const long hi = static_cast<long>(full), lo = -hi - 1;
    if (r > hi) r = hi;
    if (r < lo) r = lo;
    uint8_t *p = b.data() + idx * (bits / 8u);
    p[0] = static_cast<uint8_t>(r & 0xFF);
    p[1] = static_cast<uint8_t>((r >> 8) & 0xFF);
    if (bits == 24) p[2] = static_cast<uint8_t>((r >> 16) & 0xFF);
}

inline int32_t get(const Bytes &b, size_t idx, uint16_t bits) {
    const uint8_t *p = b.data() + idx * (bits / 8u);
    if (bits == 24) {
        uint32_t u = p[0] | (p[1] << 8) | (static_cast<uint32_t>(p[2]) << 16);
        return static_cast<int32_t>(u << 8) >> 8;
    }
    return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
}

inline double db(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

/* A two-pole resonator (formant) */
struct Reson {
    double a1 = 0, a2 = 0, g = 0, y1 = 0, y2 = 0;
    void set(double freq, double bw, double rate) {
        const double r = std::exp(-kPi * bw / rate);
        a1 = 2.0 * r * std::cos(2.0 * kPi * freq / rate);
        a2 = -r * r;
        g = 1.0 - r;
    }
    double run(double x) {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

inline Bytes speech(const cassette::AudioFormat &f, size_t frames, double noise_dbfs = -70.0, uint64_t seed = 7) {
    Bytes b(frames * f.channels * (f.bits / 8u));
    Rng rng(seed);
    Reson f1, f2, f3;
    const double vowels[][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480},
                                {660, 1720, 2410}, {490, 1350, 1690}, {640, 1190, 2390}};
    const size_t syl = f.rate / 4;  /* a syllable: 250 ms */
    double phase = 0, env = 0, target_env = 0, lp = 0;
    int vowel = 0;
    double pitch = 140;
    const double noise = db(noise_dbfs);
    for (size_t i = 0; i < frames; ++i) {
        const size_t s = i / syl, k = i % syl;
        if (k == 0) {
            vowel = static_cast<int>(rng.next() % 7);
            f1.set(vowels[vowel][0], 80, f.rate);
            f2.set(vowels[vowel][1], 100, f.rate);
            f3.set(vowels[vowel][2], 140, f.rate);
            /* words of 2-4 syllables, a pause syllable between words, a longer pause every ~3 s */
            const bool pause = (s % 4 == 3) || (s % 13 == 12) || (s % 13 == 11 && (rng.next() & 1));
            target_env = pause ? 0.0 : 0.5 + 0.5 * rng.uniform();
            pitch = 100 + 120 * rng.uniform();
        }
        const double t = static_cast<double>(k) / syl;
        const double shape = std::sin(kPi * t);            /* rise and fall within the syllable */
        env += (target_env * shape - env) * 0.002;          /* smoothed */
        phase += (pitch * (1.0 + 0.1 * (t - 0.5))) / f.rate; /* gliding pitch */
        double src = 0;
        if (phase >= 1.0) {
            phase -= 1.0;
            src = 1.0; /* glottal pulse */
        }
        lp += (src - lp) * 0.3;
        const double breath = 0.02 * rng.gauss();
        const double v = f1.run(lp + breath) * 1.0 + f2.run(lp + breath) * 0.5 + f3.run(lp + breath) * 0.25;
        const double x = 2.2 * env * v;
        for (uint16_t c = 0; c < f.channels; ++c)
            put(b, i * f.channels + c, f.bits, x * (c ? 0.97 : 1.0) + noise * rng.gauss());
    }
    return b;
}

inline Bytes music(const cassette::AudioFormat &f, size_t frames, double noise_dbfs = -70.0, uint64_t seed = 11) {
    Bytes b(frames * f.channels * (f.bits / 8u));
    Rng rng(seed);
    /* chords (semitones above A2 = 110 Hz): I - vi - IV - V */
    const int chords[4][4] = {{3, 7, 10, 15}, {0, 3, 7, 12}, {-2, 5, 8, 12}, {-4, 2, 5, 10}};
    const size_t bar = f.rate * 2; /* a chord per 2 s */
    const size_t beat = f.rate / 2;
    const double noise = db(noise_dbfs);
    double hat_lp = 0;
    for (size_t i = 0; i < frames; ++i) {
        const int ch = static_cast<int>((i / bar) % 4);
        const double tb = static_cast<double>(i % beat) / f.rate; /* time since the beat */
        const double tt = static_cast<double>(i) / f.rate;
        double l = 0, r = 0;
        for (int v = 0; v < 4; ++v) {
            const double freq = 220.0 * std::pow(2.0, chords[ch][v] / 12.0) * (1.0 + 0.0007 * v);
            double x = 0;
            for (int h = 1; h <= 6; ++h) x += std::sin(2 * kPi * freq * h * tt + v) / (h * h);
            const double e = 0.12 * (0.4 + 0.6 * std::exp(-2.5 * static_cast<double>(i % bar) / f.rate));
            const double pan = 0.2 + 0.2 * v;
            l += x * e * (1.0 - pan);
            r += x * e * pan;
        }
        const double bass_f = 55.0 * std::pow(2.0, chords[ch][0] / 12.0);
        const double bass = 0.25 * std::sin(2 * kPi * bass_f * tt) * std::exp(-3.0 * tb);
        hat_lp += (rng.gauss() - hat_lp) * 0.9; /* bright noise */
        const double hat = 0.05 * hat_lp * std::exp(-40.0 * std::fmod(tb, 0.25));
        l += bass + hat * 0.7;
        r += bass + hat;
        for (uint16_t c = 0; c < f.channels; ++c) {
            const double x = f.channels == 1 ? 0.5 * (l + r) : (c ? r : l);
            put(b, i * f.channels + c, f.bits, x + noise * rng.gauss());
        }
    }
    return b;
}

/* A plain ramp of distinct samples: what the fakes must receive unchanged. */
inline Bytes ramp(const cassette::AudioFormat &f, size_t frames) {
    Bytes b(frames * f.channels * (f.bits / 8u));
    const double full = f.bits == 24 ? 8388607.0 : 32767.0;
    for (size_t i = 0; i < frames * f.channels; ++i)
        put(b, i, f.bits, (static_cast<double>((i * 7919) % 65536) - 32768.0) / 32768.0 * (full - 1) / full);
    return b;
}

}  // namespace sig
