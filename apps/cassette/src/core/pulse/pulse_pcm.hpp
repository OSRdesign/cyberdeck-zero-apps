/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound server: a capture stream seen as an AlsaPcm, so the existing capture thread (alsa/capture.hpp: the
 * period loop, the sink, the meter callback, the lost-card handling) runs on it unchanged.
 *
 *   readi    -> PulseCapture::read with a short wait (read_timeout_ms): frames, 0 while none came (the thread naps and
 *               asks again: Connecting, Suspended), or, once the stream has ended and its last frames were read, -ENODEV
 *               for a device that went away, moved or was disconnected (Capture stops itself with CaptureError::Unplugged,
 *               the controller's DeviceLost path), -EIO for any other end.
 *   recover  -> nothing to recover (the server handles overruns): returns the error, so Capture ends the stream.
 *   prepare  -> 0 while the stream lives, -ENODEV after its end.
 *
 * The stream is shared: the owner keeps its own std::shared_ptr to poll the events and the identity on its own thread
 * while the capture thread reads. Capture closes and drops its PCM by itself when the device is lost, so a raw pointer
 * would dangle exactly when the events matter most (Killed, Moved); the owner's share keeps the stream (closed, its
 * events still readable) alive.
 */
#pragma once

#include "../alsa/open_policy.hpp"
#include "pulse_api.hpp"

#include <memory>

namespace cassette {

/* The errno an ended stream is reported with: -ENODEV for kPulseKilled / kPulseNoDevice / kPulseMoved /
 * kPulseDisconnected, -EIO otherwise. */
int pulse_end_errno(int pulse_err);

class PulseCapturePcm final : public AlsaPcm {
public:
    explicit PulseCapturePcm(std::shared_ptr<PulseCapture> stream, int read_timeout_ms = 50);
    ~PulseCapturePcm() override;
    long readi(void *buf, size_t frames) override;
    int recover(int err) override { return err < 0 ? err : -EIO; }
    int prepare() override;
    void close() override;
    const std::shared_ptr<PulseCapture> &stream() const { return s_; }

private:
    std::shared_ptr<PulseCapture> s_;
    int timeout_ms_;
};

/* An OpenResult for Capture: status Ok, via_server, the stream's rate and channels as 16-bit frames (no conversion), the
 * device = the source name, the PCM = a PulseCapturePcm sharing the stream (keep your own share for the events). */
OpenResult pulse_open_result(std::shared_ptr<PulseCapture> stream, int read_timeout_ms = 50);

}  // namespace cassette
