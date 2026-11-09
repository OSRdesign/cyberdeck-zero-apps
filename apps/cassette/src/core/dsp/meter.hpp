/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP: the level meter. The capture thread feeds it once per period with interleaved S16_LE or S24_3LE
 * frames; the UI thread reads `snapshot()` (30 Hz) without any lock (a seqlock over atomic words: the writer
 * never waits, a reader retries only while a publish is in progress).
 *
 * Conventions (documented here, used by the tests):
 *   - dBFS reference is the largest positive sample (32767 or 8388607). Magnitudes are clamped to it, so both
 *     +32767 and -32768 read exactly 0.0 dBFS. A sine of amplitude A reads 20*log10(A/32767) sample peak.
 *   - RMS is the plain root mean square of the period (no +3 dB "AES17" correction): a -20 dBFS sine reads
 *     -23.01 dBFS RMS.
 *   - Silence (a zero peak or RMS) reads the floor, kFloorDb = -120 dBFS, instead of -inf. -120 is below the
 *     quantisation floor of 24-bit audio (-138 dBFS full range, but no card reaches it) and far below anything the
 *     15-step bar or the UI shows, and it keeps every value a finite float.
 *   - Clip counter: samples at full scale (>= +max or == min), cumulative since reset().
 *   - PEAK lamp: lit when a sample reaches -1 dBFS or above, stays lit 1 s after the last such sample.
 *   - Peak hold: holds the highest period peak for 1.5 s, then falls at kHoldFallDbPerSec (20 dB/s, not fixed by the
 *     plan) until it meets the current peak.
 *   - Bar: 15 steps over -40..0 dBFS; step i (0..14) is lit when peak_dBFS >= -40 + i*40/14. `bar` is the number of
 *     lit steps (0..15) for the period's sample peak.
 *   - Target zone -18..-6 dBFS, per channel: High when a period peak above -6 dBFS occurred within the last 1.5 s
 *     (the peak-hold time; not fixed by the plan); else Low when no period peak reached -18 dBFS within the last 3 s
 *     (also right after reset(), before any loud enough period); else InZone.
 *   - Time: feed(data, n, now_us) takes an explicit clock (tests use a fake one); feed(data, n) uses the audio clock
 *     (frames fed / rate), which is deterministic and stops while no audio arrives.
 *   - Channels: the first kMaxChannels (2: mono = one bar, stereo = two) are metered; further channels are ignored.
 *
 * Not thread-safe on the writer side: one capture thread feeds; any number of readers may call snapshot().
 * No allocation after construction. LVGL-free.
 */
#pragma once

#include "../frame_sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cassette {

enum class Zone : uint8_t { Low = 0, InZone = 1, High = 2 };

struct ChannelLevel {
    float peak_db = -120.0f;  /* sample peak of the last period, dBFS */
    float rms_db = -120.0f;   /* RMS of the last period, dBFS */
    float hold_db = -120.0f;  /* peak hold (white tick), dBFS */
    uint32_t clips = 0;       /* samples at full scale since reset() */
    uint8_t bar = 0;          /* lit steps 0..15 for peak_db */
    uint8_t hold_bar = 0;     /* the step the hold tick sits on (lit-step count for hold_db) */
    bool peak_lamp = false;   /* PEAK lamp */
    Zone zone = Zone::Low;
};

struct MeterSnapshot {
    static constexpr int kMaxChannels = 2;
    uint16_t channels = 0;    /* metered channels (1 or 2), 0 before the first period */
    uint64_t periods = 0;     /* periods fed since reset() */
    uint64_t frames = 0;      /* frames fed since reset() */
    ChannelLevel ch[kMaxChannels];
};

class Meter {
public:
    static constexpr float kFloorDb = -120.0f;
    static constexpr float kBarMinDb = -40.0f;
    static constexpr int kBarSteps = 15;
    static constexpr float kZoneLowDb = -18.0f;
    static constexpr float kZoneHighDb = -6.0f;
    static constexpr float kLampDb = -1.0f;
    static constexpr uint64_t kHoldUs = 1500000;
    static constexpr float kHoldFallDbPerSec = 20.0f;
    static constexpr uint64_t kLampUs = 1000000;
    static constexpr uint64_t kLowWindowUs = 3000000;
    static constexpr uint64_t kHighWindowUs = 1500000;

    explicit Meter(const AudioFormat &fmt);

    const AudioFormat &format() const { return fmt_; }

    /* Capture thread. Ignores unsupported formats (bits other than 16 or 24). */
    void feed(const void *frames, size_t nframes);
    void feed(const void *frames, size_t nframes, uint64_t now_us);
    /* A feeder that has levels instead of samples (the playback meter: the Player's sample peak per period, fed by the
     * controller's service thread). `peak_db` per channel (clamped to kFloorDb..0); the same bar, hold and zone rules as
     * feed(); RMS reads the peak, no clips are counted and the PEAK lamp stays off (it is the recording's clip warning). */
    void feed_peaks(const float *peak_db, uint16_t channels, uint64_t now_us);

    /* Capture thread (or while nothing feeds): clears counters, hold, lamp and zone history. */
    void reset();

    /* Any thread, lock-free. */
    MeterSnapshot snapshot() const;

    /* Helpers, exposed for the UI and tests. */
    static float to_db(double magnitude_ratio);    /* ratio of full scale -> dBFS, floored at kFloorDb */
    static uint8_t bar_steps(float db);           /* lit-step count 0..15 */

private:
    struct ChanState {
        float held_db;
        uint64_t held_at;
        uint64_t lamp_at;
        uint64_t reach_low_at;    /* last period whose peak >= kZoneLowDb */
        uint64_t over_high_at;    /* last period whose peak > kZoneHighDb */
        bool lamp_seen, low_seen, high_seen;
        uint32_t clips;
    };

    void update(uint16_t n, const float *pk_db, const float *rms_db, const bool *lamp_hit, const uint32_t *clips,
                uint64_t now_us);
    void publish(const MeterSnapshot &s);

    AudioFormat fmt_;
    uint16_t nch_;
    uint64_t frames_total_ = 0;
    uint64_t periods_ = 0;
    ChanState st_[MeterSnapshot::kMaxChannels];

    static constexpr size_t kWords = (sizeof(MeterSnapshot) + 3) / 4;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords];
};

}  // namespace cassette
