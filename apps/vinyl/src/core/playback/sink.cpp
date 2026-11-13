/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "sink.hpp"

#include <cerrno>
#include <utility>

namespace vinyl {

uint32_t playback_buffer_frames(uint32_t rate) {
    uint32_t half = rate / 2;
    uint32_t periods = (half + kPlaybackPeriod - 1) / kPlaybackPeriod;
    if (periods < 2) periods = 2;
    return periods * kPlaybackPeriod;
}

SampleFormat format_for_bits(uint16_t bits) {
    switch (bits) {
    case 24: return SampleFormat::S24_3LE;
    case 32: return SampleFormat::S32_LE;
    default: return SampleFormat::S16_LE;
    }
}

AlsaPlaybackSink::AlsaPlaybackSink(AlsaApi &api, std::string device) : api_(api), device_(std::move(device)) {}

AlsaPlaybackSink::~AlsaPlaybackSink() { close(); }

namespace {
std::string plug_variant(const std::string &dev) {
    if (dev.compare(0, 3, "hw:") == 0) return "plug" + dev;
    return "";
}
} // namespace

int AlsaPlaybackSink::open(const AudioFormat &src, SinkFormat &granted) {
    close();
    HwRequest req;
    req.rate = src.rate;
    req.channels = src.channels ? src.channels : 1;
    req.format = format_for_bits(src.bits);
    req.period_frames = period_req_ ? period_req_ : kPlaybackPeriod;
    req.buffer_frames = buffer_req_ ? buffer_req_ : playback_buffer_frames(src.rate);

    std::string dev = device_;
    const std::string plug = plug_variant(device_);
    if (!plug.empty()) {
        PlaybackOffer offer;
        if (api_.query_playback(device_, offer) == 0) {
            if (offer.min_channels && req.channels < offer.min_channels) req.channels = offer.min_channels;
            if (offer.max_channels && req.channels > offer.max_channels) req.channels = offer.max_channels;
            if (!offer.has_format(req.format)) {
                if (offer.has_format(SampleFormat::S16_LE)) req.format = SampleFormat::S16_LE;
                else if (!offer.formats.empty()) req.format = offer.formats.front();
            }
            if (!offer.has_rate(req.rate)) dev = plug;
        }
    }
    HwGranted hw;
    int r = api_.open_playback(dev, req, pcm_, hw);
    if (r == -EINVAL && dev != plug && !plug.empty()) {
        dev = plug;
        r = api_.open_playback(dev, req, pcm_, hw);
    }
    if (r == -EINVAL && plug.empty() && req.channels == 1) { /* a server device that wants stereo */
        req.channels = 2;
        r = api_.open_playback(dev, req, pcm_, hw);
    }
    if (r < 0) {
        pcm_.reset();
        return r;
    }
    opened_ = dev;
    granted.rate = hw.rate;
    granted.channels = hw.channels;
    granted.format = hw.format;
    granted.period_frames = hw.period_frames ? hw.period_frames : kPlaybackPeriod;
    granted.buffer_frames = hw.buffer_frames;
    return 0;
}

long AlsaPlaybackSink::write(const void *frames, size_t nframes) {
    return pcm_ ? pcm_->writei(frames, nframes) : -ENODEV;
}

int AlsaPlaybackSink::recover(int err) { return pcm_ ? pcm_->recover(err) : -ENODEV; }

long AlsaPlaybackSink::delay() {
    if (!pcm_) return 0;
    long d = pcm_->delay();
    return d < 0 ? 0 : d;
}

/* After snd_pcm_drop and snd_pcm_drain the PCM is in SETUP: prepare it so the next write starts it again. */
void AlsaPlaybackSink::drop() {
    if (!pcm_) return;
    pcm_->drop();
    pcm_->prepare();
}

void AlsaPlaybackSink::drain() {
    if (!pcm_) return;
    pcm_->drain();
    pcm_->prepare();
}

void AlsaPlaybackSink::close() {
    if (pcm_) pcm_->close();
    pcm_.reset();
    opened_.clear();
}

} // namespace vinyl
