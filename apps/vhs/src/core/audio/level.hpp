/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Sound-on lamp and the silence detector (D38, brief 6.6). Fed with the camera microphone's S16 frames on the
 * audio thread; read through atomics by the UI.
 *   lamp     on when the RMS of a 200 ms window exceeds -50 dBFS; off after 1 s below (tune on board test B6)
 *   silence  3 s of exactly zero samples (Cassette's digital-silence rule): once per scene, the toast
 *            "No sound from the camera microphone."
 * Pure apart from the atomics; time comes from the sample count. LVGL-free.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vhs {

class SoundLevel {
public:
    void reset(int rate, int channels);
    /* Interleaved S16 frames (the audio thread). */
    void feed(const std::int16_t *frames, std::size_t n);
    bool lamp() const { return lamp_.load(); }
    /* True once per scene when 3 s of exact zeros went by. */
    bool silence_event() { return silence_.exchange(false); }
    bool silence_seen() const { return silence_seen_.load(); }
    double last_dbfs() const { return last_dbfs_.load(); }

    static constexpr double kOnDbfs = -50.0;
    static constexpr int kWindowMs = 200, kOffMs = 1000, kSilenceMs = 3000;

private:
    int rate_ = 16000, channels_ = 1;
    std::uint64_t win_frames_ = 0, win_target_ = 3200;
    double win_sum_ = 0;
    std::uint64_t below_frames_ = 0;
    std::uint64_t zero_frames_ = 0;
    std::atomic<bool> lamp_{false};
    std::atomic<bool> silence_{false};
    std::atomic<bool> silence_seen_{false};
    std::atomic<double> last_dbfs_{-120.0};
};

} // namespace vhs
