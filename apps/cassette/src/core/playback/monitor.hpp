/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: live monitoring of the microphone (the card's own playback side). LVGL-free.
 *
 *   capture thread -> ProcessingSink (DC blocker, SOFT gain, ...) -> MonitorTap::write -> lock-free Ring (0.25 s)
 *   monitor thread -> Ring::read -> monitor level (software gain) -> channel / format conversion -> PlaybackSink
 *
 * The tap sees the SAME processed samples that go to the recording. MonitorTap::write never blocks, never locks and never
 * allocates; when the ring is full the frames are dropped (counted). A monitor that fails, blocks or is slow therefore
 * cannot cost the take a single frame. Monitor::stop() waits at most kMonitorStopWaitMs for the thread: a sink whose write
 * never returns is abandoned (the thread keeps its own share of the state and closes the sink when the write finally
 * returns).
 *
 * Level: percent 0..100 mapped to a gain of (pct/100)^2 (a perceptual curve: the slider feels even), applied as a linear
 * ramp across each block, so a change never clicks. 0 % keeps the stream open and plays silence (no reopen when the level
 * comes back up, and the card never sees the stream vanish while the user drags the slider).
 *
 * Latency: the sink is asked for a small buffer (4 periods of 256 frames, about 21 ms at 48 kHz) and is primed with 512 frames
 * of silence (the capture delivers 1024 frames at a time: the margin keeps jitter from becoming underruns); the thread keeps the
 * device queue under kMonitorMaxQueueMs and the ring backlog under kMonitorMaxBacklogMs: on an underrun it recovers and goes
 * on, on a backlog it drops the oldest audio ("resync"): the delay never accumulates. Buffering granted above the target is
 * reported in stats() (latency_ms), not refused.
 */
#pragma once

#include "../frame_sink.hpp"
#include "../ring.hpp"
#include "sink.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace cassette {

constexpr int kMonitorLevelDefault = 50;
constexpr uint32_t kMonitorPeriodFrames = 256;
constexpr uint32_t kMonitorBufferFrames = 1024;
constexpr uint32_t kMonitorPrefillFrames = 512; /* silence written first and after an underrun: jitter margin (10.7 ms at 48 kHz) */
constexpr double kMonitorMaxQueueMs = 60.0;
constexpr double kMonitorMaxBacklogMs = 60.0;
constexpr unsigned kMonitorStopWaitMs = 400;

/* The perceptual curve: (pct/100)^2, pct clamped to 0..100. */
float monitor_gain(int pct);
/* Settings values: 0..100 in steps of 5 (the nearest step). */
int monitor_level_clamp(int pct);

/* The producer end, called on the capture thread. */
class MonitorTap final : public FrameSink {
public:
    explicit MonitorTap(const AudioFormat &fmt, double seconds = 0.25);
    /* Always reports every frame as accepted (a full ring drops, counted in dropped()). Does nothing while not active. */
    size_t write(const void *frames, size_t nframes) override;
    void set_active(bool on) { active_.store(on, std::memory_order_release); }
    bool active() const { return active_.load(std::memory_order_acquire); }
    uint64_t dropped() const { return ring_->frames_dropped(); }
    uint64_t frames_in() const { return ring_->frames_written(); }
    const AudioFormat &format() const { return fmt_; }
    std::shared_ptr<Ring> ring() const { return ring_; }

private:
    AudioFormat fmt_;
    std::shared_ptr<Ring> ring_;
    std::atomic<bool> active_{false};
};

struct MonitorStats {
    bool running = false;
    int error = 0;                  /* the negative error that ended the thread by itself, else 0 */
    uint64_t frames_played = 0;     /* frames written to the sink */
    uint64_t underruns = 0;         /* -EPIPE recovered */
    uint64_t resyncs = 0;           /* times audio was dropped to keep the delay small */
    uint64_t frames_dropped_in = 0; /* the tap's ring was full */
    uint32_t sink_period = 0, sink_buffer = 0, sink_rate = 0;
    unsigned sink_channels = 0;
    double latency_ms = 0;          /* the sink's granted buffer, in ms */
    bool stalled_abandoned = false; /* stop() gave up on a sink that did not return */
};

struct MonitorShared;

class Monitor {
public:
    /* `tap` supplies the audio (its format is the source format). */
    explicit Monitor(MonitorTap &tap);
    ~Monitor();
    Monitor(const Monitor &) = delete;
    Monitor &operator=(const Monitor &) = delete;

    /* Opens `sink` for the tap's format asking for a small buffer, starts the thread and activates the tap. 0, or a negative
     * error: the sink could not be opened or cannot play this signal (the monitor stays off, the sink is closed). */
    int start(std::unique_ptr<PlaybackSink> sink);
    /* Deactivates the tap, ends the thread (waits at most kMonitorStopWaitMs), closes the sink. Safe twice and when the
     * thread ended by itself. */
    void stop();
    bool running() const;
    /* Any thread, lock-free: percent 0..100 (clamped). */
    void set_level_pct(int pct);
    int level_pct() const;
    MonitorStats stats() const;

private:
    MonitorTap &tap_;
    std::shared_ptr<MonitorShared> sh_;
};

} // namespace cassette
