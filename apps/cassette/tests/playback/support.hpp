/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback tests: a temp folder, synthetic WAV files (ramps and a counter pattern: never a recording), a fake
 * clock and a recording PlaybackSink that models the device queue (delay, drop, drain), underruns and a lost device.
 */
#pragma once

#include "fileops.hpp"
#include "sink.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tp {

using namespace cassette;

struct TempDir {
    std::string path;
    TempDir() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-play-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        if (!mkdtemp(b.data())) std::abort();
        path = b.data();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    std::string file(const std::string &n) const { return path + "/" + n; }
};

/* A ramp: sample i of channel c. Wraps inside the 16-bit range; channel 1 is the negated ramp, channel 2+ offset. */
inline int32_t ramp(uint64_t i, unsigned c = 0, unsigned bits = 16) {
    int32_t v = static_cast<int32_t>(i % 60000) - 30000 + static_cast<int32_t>(((i * 2654435761u) >> 13) % 17) - 8; /* a ramp with ripple */
    if (c == 1) v = -v - 1;
    if (c >= 2) v = v / 2 + static_cast<int32_t>(c);
    if (bits == 24) v = v * 256 + static_cast<int32_t>(i % 251);
    if (bits == 32) v = v * 65536 + static_cast<int32_t>(i % 65521);
    return v;
}

/* Interleaved samples of the ramp. */
inline std::vector<int32_t> ramp_samples(uint64_t frames, unsigned ch, unsigned bits = 16) {
    std::vector<int32_t> v;
    v.reserve(frames * ch);
    for (uint64_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < ch; ++c) v.push_back(ramp(i, c, bits));
    return v;
}

enum class HeaderKind { Final, Placeholder, Oversized, Short };

/* Writes a WAV file with Cassette's header. Final: sizes right. Placeholder: the header written for zero audio and never
 * updated (a crash or a take still open). Oversized: claims `extra` frames more than the file holds. Short: claims `extra`
 * frames fewer than the file holds (trailing bytes after the declared data). */
inline void write_wav(const std::string &path, const AudioFormat &fmt, const std::vector<int32_t> &samples,
                      HeaderKind kind = HeaderKind::Final, uint64_t extra = 0) {
    const unsigned bytes = fmt.bits / 8;
    const uint64_t data = samples.size() * bytes;
    const uint64_t align = static_cast<uint64_t>(fmt.channels) * bytes;
    uint64_t declared = data;
    if (kind == HeaderKind::Placeholder) declared = 0;
    if (kind == HeaderKind::Oversized) declared = data + extra * align;
    if (kind == HeaderKind::Short) declared = data - extra * align;
    BwfInfo info;
    std::vector<unsigned char> buf = build_header(fmt, info, declared);
    buf.reserve(buf.size() + data);
    for (int32_t s : samples) {
        uint32_t u = static_cast<uint32_t>(s);
        for (unsigned b = 0; b < bytes; ++b) buf.push_back(static_cast<unsigned char>((u >> (8 * b)) & 0xFF));
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

inline AudioFormat afmt(uint32_t rate, uint16_t ch, uint16_t bits) {
    AudioFormat f;
    f.rate = rate;
    f.channels = ch;
    f.bits = bits;
    return f;
}

class FakeClock final : public Clock {
public:
    uint64_t ms = 1000;
    uint64_t mono_ms() override { return ms; }
    int64_t wall_s() override { return 1762560000 + static_cast<int64_t>(ms / 1000); }
    void sleep_ms(uint32_t d) override { ms += d; }
};

inline int32_t decode(const unsigned char *p, unsigned bytes) {
    uint32_t u = 0;
    for (unsigned b = 0; b < bytes; ++b) u |= static_cast<uint32_t>(p[b]) << (8 * b);
    if (bytes < 4 && (u & (1u << (8 * bytes - 1)))) u |= ~((1u << (8 * bytes)) - 1);
    return static_cast<int32_t>(u);
}

/* A recording sink. `heard` holds the decoded samples the device has played or still holds (interleaved); a drop removes
 * the queued tail, as a real device would never play it. Thread-safe (the TSan test reads it while the player runs). */
class FakeSink final : public PlaybackSink {
public:
    /* configuration */
    int open_error = 0;
    unsigned force_channels = 0;  /* grant this many channels whatever the source has */
    bool force_s16 = false;       /* grant S16 whatever the source has */
    long latency = 0;             /* delay() reports up to this many frames */
    std::vector<uint64_t> underrun_at; /* -EPIPE once when this many frames have been written in total */
    uint64_t lost_at = UINT64_MAX;     /* -ENODEV from this total on */
    size_t max_write = 0;              /* short writes: at most this many frames per call */
    unsigned sleep_us = 0;             /* each write sleeps (the threads test) */

    int open(const AudioFormat &src, SinkFormat &granted) override {
        std::lock_guard<std::mutex> g(mu_);
        ++opens_;
        if (open_error) return open_error;
        fmt_.rate = src.rate;
        fmt_.channels = force_channels ? force_channels : src.channels;
        fmt_.format = force_s16 ? SampleFormat::S16_LE : format_for_bits(src.bits);
        fmt_.period_frames = kPlaybackPeriod;
        fmt_.buffer_frames = playback_buffer_frames(src.rate);
        granted = fmt_;
        open_ = true;
        dead_ = false;
        since_ = 0;
        return 0;
    }
    long write(const void *frames, size_t n) override {
        if (sleep_us) std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        std::lock_guard<std::mutex> g(mu_);
        ++writes_;
        if (!open_) return -EBADFD;
        if (dead_) return -ENODEV;
        if (xrun_) return -EPIPE;
        for (size_t k = 0; k < underrun_at.size(); ++k)
            if (underrun_at[k] <= total_) {
                underrun_at.erase(underrun_at.begin() + static_cast<long>(k));
                xrun_ = true;
                return -EPIPE;
            }
        if (total_ >= lost_at) {
            dead_ = true;
            return -ENODEV;
        }
        uint64_t lim = n;
        if (max_write) lim = std::min<uint64_t>(lim, max_write);
        for (uint64_t u : underrun_at)
            if (u > total_) lim = std::min<uint64_t>(lim, u - total_);
        if (lost_at > total_) lim = std::min<uint64_t>(lim, lost_at - total_);
        const unsigned bytes = format_bytes(fmt_.format);
        const unsigned char *p = static_cast<const unsigned char *>(frames);
        for (uint64_t i = 0; i < lim * fmt_.channels; ++i) heard_.push_back(decode(p + i * bytes, bytes));
        total_ += lim;
        since_ += lim;
        return static_cast<long>(lim);
    }
    int recover(int err) override {
        std::lock_guard<std::mutex> g(mu_);
        ++recovers_;
        if (err == -EPIPE && xrun_) {
            xrun_ = false;
            return 0;
        }
        return err;
    }
    long delay() override {
        std::lock_guard<std::mutex> g(mu_);
        return static_cast<long>(std::min<uint64_t>(static_cast<uint64_t>(latency), since_));
    }
    void drop() override {
        std::lock_guard<std::mutex> g(mu_);
        ++drops_;
        const uint64_t d = std::min<uint64_t>(static_cast<uint64_t>(latency), since_);
        heard_.resize(heard_.size() - d * fmt_.channels);
        since_ = 0;
    }
    void drain() override {
        std::lock_guard<std::mutex> g(mu_);
        ++drains_;
        since_ = 0;
    }
    void close() override {
        std::lock_guard<std::mutex> g(mu_);
        if (open_) ++closes_;
        open_ = false;
    }

    std::vector<int32_t> heard() const {
        std::lock_guard<std::mutex> g(mu_);
        return heard_;
    }
    size_t heard_frames() const {
        std::lock_guard<std::mutex> g(mu_);
        return fmt_.channels ? heard_.size() / fmt_.channels : 0;
    }
    void clear_heard() {
        std::lock_guard<std::mutex> g(mu_);
        heard_.clear();
    }
    unsigned opens() const { std::lock_guard<std::mutex> g(mu_); return opens_; }
    unsigned drops() const { std::lock_guard<std::mutex> g(mu_); return drops_; }
    unsigned drains() const { std::lock_guard<std::mutex> g(mu_); return drains_; }
    unsigned closes() const { std::lock_guard<std::mutex> g(mu_); return closes_; }
    unsigned writes() const { std::lock_guard<std::mutex> g(mu_); return writes_; }
    bool is_open() const { std::lock_guard<std::mutex> g(mu_); return open_; }
    SinkFormat format() const { std::lock_guard<std::mutex> g(mu_); return fmt_; }

private:
    mutable std::mutex mu_;
    SinkFormat fmt_;
    std::vector<int32_t> heard_;
    uint64_t total_ = 0, since_ = 0;
    bool open_ = false, dead_ = false, xrun_ = false;
    unsigned opens_ = 0, drops_ = 0, drains_ = 0, closes_ = 0, writes_ = 0, recovers_ = 0;
};

/* Expected output of a box filter: the rounded mean (halves away from zero) of samples [a, b) of one channel. */
inline int32_t box(const std::vector<int32_t> &s, unsigned ch, unsigned c, uint64_t a, uint64_t b) {
    int64_t sum = 0;
    for (uint64_t i = a; i < b; ++i) sum += s[i * ch + c];
    const int64_t k = static_cast<int64_t>(b - a);
    return static_cast<int32_t>(sum >= 0 ? (sum + k / 2) / k : -((-sum + k / 2) / k));
}

/* Interleaved samples [a, b) frames. */
inline std::vector<int32_t> slice(const std::vector<int32_t> &s, unsigned ch, uint64_t a, uint64_t b) {
    return std::vector<int32_t>(s.begin() + static_cast<long>(a * ch), s.begin() + static_cast<long>(b * ch));
}

} // namespace tp
