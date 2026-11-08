/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP tests: synthetic signals only (sines at known dBFS, squares, silence, offsets), interleaved
 * S16_LE or S24_3LE, plus a fake FrameSink that records what it accepts.
 */
#pragma once

#include "frame_sink.hpp"
#include "pcm.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace sig {

using Bytes = std::vector<uint8_t>;
constexpr double kPi = 3.14159265358979323846;

/* dBFS relative to the largest positive sample (the meter's reference) -> amplitude in LSB */
inline double amp(uint16_t bits, double dbfs) { return double(cassette::pcm::max_pos(bits)) * std::pow(10.0, dbfs / 20.0); }

inline void put(Bytes &b, size_t idx, uint16_t bits, double v) {
    long r = std::lround(v);
    if (r > cassette::pcm::max_pos(bits)) r = cassette::pcm::max_pos(bits);
    if (r < cassette::pcm::min_neg(bits)) r = cassette::pcm::min_neg(bits);
    cassette::pcm::write(b.data() + idx * cassette::pcm::bytes_per_sample(bits), bits, int32_t(r));
}

inline int32_t get(const Bytes &b, size_t idx, uint16_t bits) {
    return cassette::pcm::read(b.data() + idx * cassette::pcm::bytes_per_sample(bits), bits);
}

/* sine per channel (dbfs[c]; <= -200 means silence), plus an optional constant offset in LSB.
 * 1 kHz at 48 kHz has 48 samples a cycle and sample 12 lands on the crest exactly. */
inline Bytes sine(const cassette::AudioFormat &f, size_t frames, double freq, const std::vector<double> &dbfs,
                  double offset = 0.0, size_t start = 0) {
    Bytes b(frames * cassette::pcm::bytes_per_frame(f));
    for (size_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < f.channels; ++c) {
            const double d = dbfs[c < dbfs.size() ? c : dbfs.size() - 1];
            const double a = d <= -200 ? 0.0 : amp(f.bits, d);
            put(b, i * f.channels + c, f.bits, offset + a * std::sin(2.0 * kPi * freq * double(i + start) / f.rate));
        }
    return b;
}

/* square wave of +-a LSB (all channels) */
inline Bytes square(const cassette::AudioFormat &f, size_t frames, int32_t a, size_t half_period = 24) {
    Bytes b(frames * cassette::pcm::bytes_per_frame(f));
    for (size_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < f.channels; ++c) put(b, i * f.channels + c, f.bits, (i / half_period) % 2 ? -a : a);
    return b;
}

/* records accepted frames into a buffer reserved up front; accepts at most `capacity` frames in total */
class FakeSink : public cassette::FrameSink {
public:
    FakeSink(const cassette::AudioFormat &f, size_t capacity) : bpf_(cassette::pcm::bytes_per_frame(f)), cap_(capacity) {
        data.reserve(capacity * bpf_);
    }
    size_t write(const void *frames, size_t n) override {
        ++calls;
        const size_t room = cap_ - got;
        const size_t k = n < room ? n : room;
        const uint8_t *p = static_cast<const uint8_t *>(frames);
        data.insert(data.end(), p, p + k * bpf_);
        got += k;
        return k;
    }
    Bytes data;
    size_t got = 0;
    size_t calls = 0;

private:
    size_t bpf_;
    size_t cap_;
};

}  // namespace sig
