/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the capture thread. Reads periods (1024 frames) from an open PCM, hands them to a FrameSink that never blocks and
 * to an optional per-period callback (the level meter). On an overrun (-EPIPE) it recovers, counts it and estimates the frames
 * lost. A lost card (-ENODEV, -EBADFD) stops the thread by itself: it closes the PCM, records the reason and does not spin.
 * No real-time priority (ulimit -r is 0 on the boards).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../frame_sink.hpp"
#include "alsa_api.hpp"
#include "open_policy.hpp"

namespace cassette {

enum class CaptureError { None, Unplugged, Overrun, Busy, Other };
const char *capture_error_name(CaptureError e);
/* Classifies a negative read or recover error: -ENODEV, -EBADFD, -ENOTTY are Unplugged; -EPIPE Overrun; -EBUSY Busy. */
CaptureError classify_error(int err);

struct CaptureStats {
    uint64_t frames_captured = 0;   /* frames read from the card (the sink may have refused some) */
    uint64_t frames_dropped = 0;    /* frames the sink refused */
    uint64_t frames_lost_xrun = 0;  /* ESTIMATE of frames the card produced while we were not reading (see Capture) */
    uint32_t overruns = 0;
    CaptureError last_error = CaptureError::None; /* the most recent error event, fatal or not */
    int last_errno = 0;
    CaptureError stop_reason = CaptureError::None; /* why the thread ended by itself; None = running or stopped by stop() */
    std::string message;            /* plain sentence for the last error */
    bool running = false;
};

struct CaptureOptions {
    /* Called on the capture thread with each period in the output format. Must be quick and must not block. */
    std::function<void(const void *frames, size_t nframes, const AudioFormat &fmt)> on_period;
    /* Seconds, monotonic. Default steady_clock; tests inject a fake. */
    std::function<double()> now;
    uint32_t period_frames = 1024;
};

class Capture {
public:
    /* Takes the open PCM from `opened` (status Ok). The sink must outlive the Capture. */
    Capture(OpenResult &&opened, FrameSink &sink, CaptureOptions opt = CaptureOptions());
    ~Capture();
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    /* Starts the thread (prepares the PCM first). Returns false when the PCM was closed after the card was lost, or when it
     * is already running. May be called again after stop(). */
    bool start();
    /* Asks the thread to end and joins it. Safe when it already ended by itself, and safe to call twice. The PCM stays open
     * for a later start(), unless the card was lost. */
    void stop();
    bool running() const { return running_.load(); }
    CaptureStats stats() const;
    const AudioFormat &format() const { return fmt_; }
    bool via_server() const { return via_server_; }

private:
    void run();
    void fatal(CaptureError why, int err, const std::string &msg);
    void note(CaptureError e, int err, const std::string &msg);

    std::unique_ptr<AlsaPcm> pcm_;
    FrameSink &sink_;
    CaptureOptions opt_;
    AudioFormat fmt_;
    Conversion conv_;
    HwGranted hw_;
    bool via_server_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> captured_{0}, dropped_{0}, lost_{0};
    std::atomic<uint32_t> overruns_{0};
    mutable std::mutex mu_;
    CaptureStats st_; /* error fields, under mu_ */
};

} // namespace cassette
