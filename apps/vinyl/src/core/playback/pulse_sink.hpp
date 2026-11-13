/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette playback: PlaybackSink on a sound-server playback stream (task T3 of the Bluetooth release, docs/PULSE.md "What T3
 * uses"). The Player and the Monitor use it exactly like AlsaPlaybackSink; the differences are all here:
 *
 *   - the stream is opened on ONE exact sink (set_sink, resolved by the controller from the server's list) with DONT_MOVE
 *     (T1), at the SOURCE's rate and channel count (1 or 2): the server resamples, the plughw rule does not apply. The format is
 *     always S16 (the Player converts 24-bit takes);
 *   - buffers are asked for in milliseconds (PulseSinkOptions);
 *   - an underrun is the server's (the stream's underflow count): the next write() after one returns -EPIPE once and recover()
 *     succeeds, so the Player and the Monitor count and handle it as they do ALSA's;
 *   - delay() is the stream's ring plus the server's latency report (PulsePlayback::delay_frames), which includes the device's
 *     own latency (A2DP), so the Player's counter stays what is heard;
 *   - there is no pause in the stream API: drop() discards what is queued (client ring and server buffer) and the stream stays
 *     open; the next write() starts it again. Underflows while nothing is written (a pause) are not reported as underruns;
 *   - a stream that ended (the sink went away, the connection was lost, the server moved it) makes write() fail with -ENODEV: the
 *     owner treats the device as lost and never moves to another sink. A server that accepts nothing for stuck_ms is the same
 *     (-EIO).
 *
 * One thread writes (open, write, drop, drain, close); ended(), end_code(), delay_ms() and underruns() may be called from any
 * thread meanwhile. LVGL-free.
 */
#pragma once

#include "../pulse/pulse_api.hpp"
#include "sink.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace vinyl {

struct PulseSinkOptions {
    uint32_t buffer_ms = 60;         /* the server-side target (tlength) */
    uint32_t ring_ms = 100;          /* the client ring write() fills (0: buffer_ms) */
    uint32_t period_ms = 20;         /* SinkFormat::period_frames: what the Player writes at a time */
    uint32_t ready_timeout_ms = 400; /* open() waits this long for the server; a slower one is returned still connecting */
    int write_timeout_ms = 20;       /* one write() waits this long while the ring is full */
    uint32_t stuck_ms = 3000;        /* no progress for this long: the device is gone (-EIO) */
    std::string stream_name = "Vinyl"; /* VINYL */
    std::string role = "music";
};

/* The playback options of the live monitor: a small server target and ring (study C4: about 20 ms fragments, total delay below
 * 100 ms on the same headset), the role "production". */
PulseSinkOptions pulse_monitor_sink_options();

class PulsePlaybackSink : public PlaybackSink {
public:
    explicit PulsePlaybackSink(PulseApi &api, PulseSinkOptions opt = PulseSinkOptions());
    ~PulsePlaybackSink() override;

    /* The exact sink to open (before open()); "" makes open() fail with -ENODEV. */
    void set_sink(const std::string &name);
    std::string sink_name() const;

    int open(const AudioFormat &src, SinkFormat &granted) override;
    long write(const void *frames, size_t nframes) override;
    int recover(int err) override;
    long delay() override;
    void drop() override;
    void drain() override;
    void close() override;
    void set_buffering(uint32_t, uint32_t) override {} /* the options are in milliseconds, fixed at construction */

    /* any thread */
    bool is_open() const;
    bool ended() const; /* the stream was open and has ended (killed, moved, connection lost) */
    int end_code() const;
    uint64_t underruns() const;
    /* the stream's delay (ring + server report) in ms; false (and 0) when closed */
    bool delay_ms(double &ms) const;

private:
    std::shared_ptr<PulsePlayback> get() const;

    PulseApi &api_;
    PulseSinkOptions opt_;
    mutable std::mutex mu_;
    std::string sink_;
    std::shared_ptr<PulsePlayback> pb_;
    uint32_t rate_ = 0;
    /* the writing thread's own bookkeeping */
    uint64_t seen_underruns_ = 0;
    bool fresh_ = true;
    std::chrono::steady_clock::time_point progress_;
};

}  // namespace vinyl
