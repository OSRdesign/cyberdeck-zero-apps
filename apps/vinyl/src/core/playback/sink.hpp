/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette playback: the output seam. The Player writes interleaved PCM to a PlaybackSink; AlsaPlaybackSink is the production
 * one (through the AlsaApi seam, so the PC tests use the fake sound system), tests/playback/fake_sink.hpp records what it is
 * given. A sink is used by one thread at a time (the playback thread). LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"
#include "frame_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace vinyl {

constexpr uint32_t kPlaybackPeriod = 1024; /* frames per write */

/* About 500 ms of buffer, a whole number of periods (24576 frames at 48 kHz). */
uint32_t playback_buffer_frames(uint32_t rate);

/* What the device takes. It may differ from the source: more channels (a stereo-only headset playing a mono take: the
 * Player duplicates the channel) or another sample format (the Player converts). The rate is always the source rate. */
struct SinkFormat {
    uint32_t rate = 0;
    unsigned channels = 0;
    SampleFormat format = SampleFormat::S16_LE;
    uint32_t period_frames = kPlaybackPeriod;
    uint32_t buffer_frames = 0;
};

class PlaybackSink {
public:
    virtual ~PlaybackSink() = default;
    /* Opens the output for a source format: 0 or a negative error. */
    virtual int open(const AudioFormat &src, SinkFormat &granted) = 0;
    /* Frames accepted (> 0, possibly fewer than offered; blocks while the buffer is full) or a negative error:
     * -EPIPE / -ESTRPIPE an underrun or suspend (call recover()), anything else: the device is gone. */
    virtual long write(const void *frames, size_t nframes) = 0;
    /* 0 when writing can go on after an underrun, else a negative error. */
    virtual int recover(int err) = 0;
    /* Frames written but not heard yet (>= 0). */
    virtual long delay() = 0;
    /* Discards what is queued, at once; the sink stays open and ready for writes. */
    virtual void drop() = 0;
    /* Plays out what is queued (blocks up to one buffer); the sink stays open and ready for writes. */
    virtual void drain() = 0;
    virtual void close() = 0; /* idempotent */
    /* Before open(): asks for this period and buffer (the monitor wants about 20 ms, not the player's 500 ms). The device
     * may grant more (SinkFormat says what it did). 0 = the default of the sink. A sink may ignore it. */
    virtual void set_buffering(uint32_t period_frames, uint32_t buffer_frames) {
        (void)period_frames;
        (void)buffer_frames;
    }
};

/* The sample format a WAV source of `bits` bits is played in when the device takes it. */
SampleFormat format_for_bits(uint16_t bits);

/* PlaybackSink on an AlsaApi playback device ("hw:CARD=<id>,DEV=0" or "default"). For a hw: device the offer is queried
 * first: the source channels when the device takes them, else the nearest count it takes (mono on a stereo-only headset ->
 * 2, the Player duplicates); the source format when offered, else S16, else the first one offered. When the device does
 * not take the source rate (or the hw open is refused with -EINVAL), the same card is opened through "plughw:" so ALSA
 * converts the rate: a playback-only decision (capture never uses plughw). */
class AlsaPlaybackSink : public PlaybackSink {
public:
    AlsaPlaybackSink(AlsaApi &api, std::string device);
    ~AlsaPlaybackSink() override;
    int open(const AudioFormat &src, SinkFormat &granted) override;
    long write(const void *frames, size_t nframes) override;
    int recover(int err) override;
    long delay() override;
    void drop() override;
    void drain() override;
    void close() override;
    void set_buffering(uint32_t period_frames, uint32_t buffer_frames) override {
        period_req_ = period_frames;
        buffer_req_ = buffer_frames;
    }
    const std::string &device() const { return device_; }
    const std::string &opened_device() const { return opened_; } /* the device string actually opened ("" when closed) */

private:
    AlsaApi &api_;
    std::string device_, opened_;
    uint32_t period_req_ = 0, buffer_req_ = 0; /* set_buffering: 0 = the playback defaults */
    std::unique_ptr<AlsaPcm> pcm_;
};

} // namespace vinyl
