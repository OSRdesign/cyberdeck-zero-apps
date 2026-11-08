/* SPDX-License-Identifier: MIT */
#include "meter.hpp"

#include "pcm.hpp"

#include <cmath>
#include <cstring>

namespace cassette {

Meter::Meter(const AudioFormat &fmt) : fmt_(fmt) {
    nch_ = fmt.channels < MeterSnapshot::kMaxChannels ? fmt.channels : uint16_t(MeterSnapshot::kMaxChannels);
    for (auto &w : words_) w.store(0, std::memory_order_relaxed);
    reset();
}

float Meter::to_db(double ratio) {
    if (!(ratio > 0.0)) return kFloorDb;
    const double db = 20.0 * std::log10(ratio);
    return db < kFloorDb ? kFloorDb : float(db);
}

uint8_t Meter::bar_steps(float db) {
    uint8_t n = 0;
    for (int i = 0; i < kBarSteps; ++i)
        if (db >= kBarMinDb + float(i) * (-kBarMinDb / float(kBarSteps - 1))) n = uint8_t(i + 1);
    return n;
}

void Meter::reset() {
    frames_total_ = 0;
    periods_ = 0;
    for (auto &c : st_) c = ChanState{kFloorDb, 0, 0, 0, 0, false, false, false, 0};
    MeterSnapshot s;
    s.channels = 0;
    publish(s);
}

void Meter::feed(const void *frames, size_t nframes) {
    if (fmt_.rate == 0) return;
    const uint64_t end = frames_total_ + nframes;
    feed(frames, nframes, uint64_t((long double)end * 1000000.0L / fmt_.rate));
}

void Meter::feed(const void *frames, size_t nframes, uint64_t now_us) {
    if (!pcm::supported(fmt_.bits) || nch_ == 0 || frames == nullptr) return;
    const uint8_t *p = static_cast<const uint8_t *>(frames);
    const size_t bps = pcm::bytes_per_sample(fmt_.bits);
    const size_t bpf = pcm::bytes_per_frame(fmt_);
    const int32_t maxp = pcm::max_pos(fmt_.bits), minn = pcm::min_neg(fmt_.bits);

    int32_t peak[MeterSnapshot::kMaxChannels] = {};
    double sumsq[MeterSnapshot::kMaxChannels] = {};
    uint32_t clips[MeterSnapshot::kMaxChannels] = {};
    for (size_t f = 0; f < nframes; ++f, p += bpf) {
        for (uint16_t c = 0; c < nch_; ++c) {
            const int32_t v = pcm::read(p + c * bps, fmt_.bits);
            int32_t a = v < 0 ? (v == minn ? maxp : -v) : v; /* clamp |min| to max: both read 0 dBFS */
            if (a >= maxp) ++clips[c];
            if (a > peak[c]) peak[c] = a;
            sumsq[c] += double(v) * double(v);
        }
    }
    frames_total_ += nframes;
    ++periods_;

    MeterSnapshot s;
    s.channels = nch_;
    s.periods = periods_;
    s.frames = frames_total_;
    const double fs = double(maxp);
    const int32_t lamp_level = int32_t(std::ceil(fs * std::pow(10.0, kLampDb / 20.0) - 1e-9));
    for (uint16_t c = 0; c < nch_; ++c) {
        ChanState &st = st_[c];
        ChannelLevel &out = s.ch[c];
        st.clips += clips[c];
        const float pk = to_db(double(peak[c]) / fs);
        float rms = kFloorDb;
        if (nframes) {
            const double r = std::sqrt(sumsq[c] / double(nframes)) / fs;
            rms = to_db(r > 1.0 ? 1.0 : r);
        }

        /* peak hold: hold 1.5 s, then fall at 20 dB/s down to the current peak */
        float shown = st.held_db;
        if (now_us > st.held_at + kHoldUs) {
            const double fall_s = double(now_us - st.held_at - kHoldUs) / 1e6;
            shown = float(st.held_db - kHoldFallDbPerSec * fall_s);
            if (shown < kFloorDb) shown = kFloorDb;
        }
        if (pk >= shown) {
            st.held_db = pk;
            st.held_at = now_us;
            shown = pk;
        }

        if (peak[c] >= lamp_level) { st.lamp_at = now_us; st.lamp_seen = true; }
        if (pk >= kZoneLowDb) { st.reach_low_at = now_us; st.low_seen = true; }
        if (pk > kZoneHighDb) { st.over_high_at = now_us; st.high_seen = true; }

        out.peak_db = pk;
        out.rms_db = rms;
        out.hold_db = shown;
        out.clips = st.clips;
        out.bar = bar_steps(pk);
        out.hold_bar = bar_steps(shown);
        out.peak_lamp = st.lamp_seen && now_us - st.lamp_at <= kLampUs;
        if (st.high_seen && now_us - st.over_high_at <= kHighWindowUs) out.zone = Zone::High;
        else if (!st.low_seen || now_us - st.reach_low_at > kLowWindowUs) out.zone = Zone::Low;
        else out.zone = Zone::InZone;
    }
    publish(s);
}

/* Seqlock writer (single writer), without stand-alone fences (ThreadSanitizer does not model them): the payload
 * words are release stores, so a reader whose acquire load sees any new word also sees the odd sequence value
 * written before it, and its final sequence check fails. Readers load the words with acquire, so the final sequence
 * load cannot move before them. About 30 words at 100 periods a second: negligible on the capture thread. */
void Meter::publish(const MeterSnapshot &s) {
    uint32_t buf[kWords] = {};
    std::memcpy(buf, &s, sizeof s);
    const uint32_t q = seq_.load(std::memory_order_relaxed);
    seq_.store(q + 1, std::memory_order_relaxed);
    for (size_t i = 0; i < kWords; ++i) words_[i].store(buf[i], std::memory_order_release);
    seq_.store(q + 2, std::memory_order_release);
}

MeterSnapshot Meter::snapshot() const {
    uint32_t buf[kWords];
    for (;;) {
        const uint32_t q1 = seq_.load(std::memory_order_acquire);
        if (q1 & 1u) continue; /* a publish is in progress: it takes a few dozen stores */
        for (size_t i = 0; i < kWords; ++i) buf[i] = words_[i].load(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == q1) break;
    }
    MeterSnapshot s;
    std::memcpy(&s, buf, sizeof s);
    return s;
}

}  // namespace cassette
