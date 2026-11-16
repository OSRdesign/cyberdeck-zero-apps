/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the fake camera microphone (see fake_alsa.hpp).
 */
#include "fake_alsa.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>

namespace vhs {
namespace {

std::int64_t mono_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

class FakeMicPcm : public AlsaPcm {
public:
    FakeMicPcm(const FakeMicOptions &o, unsigned channels) : o_(o), ch_(channels), t0_(mono_ns()) {}

    long readi(void *buf, size_t frames) override
    {
        if (closed_) return -EBADFD;
        const std::int64_t now = mono_ns();
        if (o_.lost_after_ms > 0 && now - t0_ >= static_cast<std::int64_t>(o_.lost_after_ms) * 1000000LL) return -ENODEV;
        if (o_.pace) {
            /* the frames are "captured" when their time has come */
            const std::int64_t due = t0_ + static_cast<std::int64_t>((done_ + frames) * 1000000000ULL / o_.rate);
            if (due > now) {
                const std::int64_t wait = due - now;
                struct timespec ts{static_cast<time_t>(wait / 1000000000LL), static_cast<long>(wait % 1000000000LL)};
                nanosleep(&ts, nullptr);
            }
        }
        auto *out = static_cast<std::int16_t *>(buf);
        for (size_t i = 0; i < frames; ++i) {
            std::int16_t v = 0;
            if (o_.mode == "tone") {
                const double t = static_cast<double>(done_ + i) / o_.rate;
                v = static_cast<std::int16_t>(std::lround(3277.0 * std::sin(2.0 * M_PI * 440.0 * t))); /* -20 dBFS */
            } else if (o_.mode == "quiet") {
                v = static_cast<std::int16_t>(((done_ + i) * 2654435761u >> 13) % 3) - 1;
            }
            for (unsigned c = 0; c < ch_; ++c) out[i * ch_ + c] = v;
        }
        done_ += frames;
        return static_cast<long>(frames);
    }
    int recover(int err) override { return err == -EPIPE ? 0 : err; }
    int status(std::int64_t &htstamp_ns, long &delay_frames) override
    {
        htstamp_ns = mono_ns();
        delay_frames = 0;
        return 0;
    }
    int prepare() override { return closed_ ? -EBADFD : 0; }
    void close() override { closed_ = true; }

private:
    FakeMicOptions o_;
    unsigned ch_;
    std::int64_t t0_;
    std::uint64_t done_ = 0;
    bool closed_ = false;
};

} // namespace

FakeMicOptions fake_mic_options(const std::string &mode)
{
    FakeMicOptions o;
    if (mode == "quiet" || mode == "silent" || mode == "busy" || mode == "none") o.mode = mode;
    return o;
}

bool FakeAlsa::available(std::string *why)
{
    (void)why;
    return true;
}

std::vector<CardInfo> FakeAlsa::cards()
{
    if (opt_.mode == "none") return {};
    CardInfo c;
    c.index = 1;
    c.id = opt_.card_id;
    c.name = "UGREEN camera";
    c.long_name = "UGREEN camera at usb-fake-" + opt_.usb_port + ", high speed";
    c.usb_id = "0bda:5830";
    c.usb_port = opt_.usb_port;
    c.capture = true;
    return {c};
}

int FakeAlsa::query_capture(const std::string &device, CaptureOffer &out)
{
    if (opt_.mode == "none" || device.find(opt_.card_id) == std::string::npos) return -ENOENT;
    if (opt_.mode == "busy") return -EBUSY;
    out = CaptureOffer();
    out.rates = {opt_.rate};
    out.min_channels = out.max_channels = opt_.channels;
    out.formats = {SampleFormat::S16_LE};
    return 0;
}

int FakeAlsa::open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                           HwGranted &granted)
{
    if (opt_.mode == "none" || device.find(opt_.card_id) == std::string::npos) return -ENOENT;
    if (opt_.mode == "busy") return -EBUSY;
    if (req.rate != opt_.rate || req.channels != opt_.channels || req.format != SampleFormat::S16_LE) return -EINVAL;
    ++opens_;
    granted.rate = req.rate;
    granted.channels = req.channels;
    granted.format = req.format;
    granted.period_frames = req.period_frames;
    granted.buffer_frames = req.buffer_frames;
    pcm = std::make_unique<FakeMicPcm>(opt_, req.channels);
    return 0;
}

int FakeAlsa::mixer_list(const std::string &card, std::vector<CtlInfo> &out)
{
    (void)card;
    out.clear();
    return 0;
}

int FakeAlsa::mixer_get(const std::string &card, const std::string &name, long &value)
{
    (void)card;
    (void)name;
    value = 0;
    return -ENOENT;
}

int FakeAlsa::mixer_set(const std::string &card, const std::string &name, long value)
{
    (void)card;
    (void)name;
    (void)value;
    return -ENOENT; /* the level is never written (D11) */
}

} // namespace vhs
