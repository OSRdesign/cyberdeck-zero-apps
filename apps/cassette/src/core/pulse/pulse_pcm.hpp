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

#include <atomic>
#include <memory>

namespace cassette {

/* The errno an ended stream is reported with: -ENODEV for kPulseKilled / kPulseNoDevice / kPulseMoved /
 * kPulseDisconnected, -EIO otherwise. */
int pulse_end_errno(int pulse_err);

/* The connecting gate of a Bluetooth microphone (study C5, C18). A headset opened from music mode delivers digital zeros for
 * about a second while its profile switches (P4): the stream says Running at once, but nothing real has arrived. While the
 * gate is closed the PCM drops the leading frames that are exactly zero (they are neither written to a take nor counted as
 * recording time) and hands over audio from the first non-zero sample on. The gate opens by itself at that sample, or the
 * owner opens it (a call profile is active and the stream has run a while: the zeros are the headset's own). Once open it
 * stays open for the life of the stream: later zeros are real silence (the stream's silence detector reports them). */
struct PulseLeadGate {
    std::atomic<bool> open{false};
    std::atomic<bool> by_audio{false};          /* it opened because a non-zero sample arrived */
    std::atomic<uint64_t> dropped_frames{0};    /* leading zero frames dropped */
};

class PulseCapturePcm final : public AlsaPcm {
public:
    /* `gate`: null passes everything through (the default). */
    explicit PulseCapturePcm(std::shared_ptr<PulseCapture> stream, int read_timeout_ms = 50,
                             std::shared_ptr<PulseLeadGate> gate = nullptr);
    ~PulseCapturePcm() override;
    long readi(void *buf, size_t frames) override;
    int recover(int err) override { return err < 0 ? err : -EIO; }
    int prepare() override;
    void close() override;
    const std::shared_ptr<PulseCapture> &stream() const { return s_; }

private:
    std::shared_ptr<PulseCapture> s_;
    int timeout_ms_;
    std::shared_ptr<PulseLeadGate> gate_;
    unsigned channels_ = 1;
};

/* An OpenResult for Capture: status Ok, via_server, the stream's rate and channels as 16-bit frames (no conversion), the
 * device = the source name, the PCM = a PulseCapturePcm sharing the stream (keep your own share for the events). */
OpenResult pulse_open_result(std::shared_ptr<PulseCapture> stream, int read_timeout_ms = 50,
                             std::shared_ptr<PulseLeadGate> gate = nullptr);

}  // namespace cassette
