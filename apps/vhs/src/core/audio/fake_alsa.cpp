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
    FakeMicPcm(const FakeMicOptions &o, unsigned channels, SampleFormat fmt = SampleFormat::S16_LE)
        : o_(o), ch_(channels), fmt_(fmt), t0_(mono_ns())
    {
    }

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
        auto *raw = static_cast<std::uint8_t *>(buf);
        for (size_t i = 0; i < frames; ++i) {
            std::int16_t v = 0;
            if (o_.mode == "tone") {
                const double t = static_cast<double>(done_ + i) / o_.rate;
                v = static_cast<std::int16_t>(std::lround(3277.0 * std::sin(2.0 * M_PI * 440.0 * t))); /* -20 dBFS */
            } else if (o_.mode == "quiet") {
                v = static_cast<std::int16_t>(((done_ + i) * 2654435761u >> 13) % 3) - 1;
            }
            for (unsigned c = 0; c < ch_; ++c) {
                const std::size_t at = i * ch_ + c;
                if (fmt_ == SampleFormat::S16_LE) {
                    out[at] = v;
                } else if (fmt_ == SampleFormat::S32_LE) {
                    const std::int32_t w = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(v)) << 16);
                    std::memcpy(raw + 4 * at, &w, 4);
                } else { /* S24_3LE */
                    const std::int32_t w = static_cast<std::int32_t>(v) * 256;
                    raw[3 * at] = static_cast<std::uint8_t>(w & 0xFF);
                    raw[3 * at + 1] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
                    raw[3 * at + 2] = static_cast<std::uint8_t>((w >> 16) & 0xFF);
                }
            }
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
    SampleFormat fmt_;
    std::int64_t t0_;
    std::uint64_t done_ = 0;
    bool closed_ = false;
};

} // namespace

FakeMicOptions fake_mic_options(const std::string &spec)
{
    FakeMicOptions o;
    std::size_t i = 0;
    bool first = true;
    while (i <= spec.size()) {
        std::size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string w = spec.substr(i, j - i);
        i = j + 1;
        if (first) {
            first = false;
            if (w == "quiet" || w == "silent" || w == "busy" || w == "none") o.mode = w;
        }
        if (w.compare(0, 7, "builtin") != 0) continue;
        FakeCard c;
        if (w == "builtin-mono") c.min_channels = c.max_channels = 1;
        else if (w == "builtin-s32") c.formats = {SampleFormat::S32_LE};
        else if (w == "builtin-busy") c.hw_busy = true;
        else if (w == "builtin-plug") c.hw_denied = true;
        else if (w == "builtin-dead") c.dead = true;
        o.cards.push_back(c);
    }
    return o;
}

std::vector<std::string> FakeAlsa::opened() const
{
    std::lock_guard<std::mutex> lock(m_);
    return opened_;
}

bool FakeAlsa::available(std::string *why)
{
    (void)why;
    return true;
}

namespace {

/* "hw:CARD=PCH,DEV=0" -> "PCH"; "plughw:CARD=PCH,DEV=0" -> "PCH"; "default" -> "" */
std::string card_of(const std::string &device)
{
    const std::size_t at = device.find("CARD=");
    if (at == std::string::npos) return "";
    const std::size_t end = device.find(',', at);
    return device.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
}

bool is_plug(const std::string &device)
{
    return device.compare(0, 6, "plughw") == 0;
}

} // namespace

std::vector<CardInfo> FakeAlsa::cards()
{
    std::vector<CardInfo> out;
    int index = 0;
    for (const FakeCard &k : opt_.cards) {
        CardInfo c;
        c.index = index++;
        c.id = k.id;
        c.name = k.name;
        c.long_name = k.name + " at " + (k.usb_port.empty() ? std::string("0000:00:1f.3") : "usb-fake-" + k.usb_port);
        c.usb_id = k.usb_id;
        c.usb_port = k.usb_port;
        c.capture = true;
        out.push_back(c);
    }
    if (opt_.mode != "none") {
        CardInfo c;
        c.index = index++;
        c.id = opt_.card_id;
        c.name = "UGREEN camera";
        c.long_name = "UGREEN camera at usb-fake-" + opt_.usb_port + ", high speed";
        c.usb_id = "0bda:5830";
        c.usb_port = opt_.usb_port;
        c.capture = true;
        out.push_back(c);
    }
    return out;
}

int FakeAlsa::query_capture(const std::string &device, CaptureOffer &out)
{
    const std::string id = card_of(device);
    for (const FakeCard &k : opt_.cards) {
        if (k.id != id) continue;
        if (k.hw_busy && !is_plug(device)) return -EBUSY;
        out = CaptureOffer();
        out.rates = k.rates;
        out.min_channels = k.min_channels;
        out.max_channels = k.max_channels;
        out.formats = k.formats;
        return 0;
    }
    if (opt_.mode == "none" || id != opt_.card_id) return -ENOENT;
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
    const std::string id = card_of(device);
    auto grant = [&](const FakeMicOptions &po, unsigned channels, SampleFormat f) {
        {
            std::lock_guard<std::mutex> lock(m_);
            opened_.push_back(device);
        }
        ++opens_;
        granted.rate = req.rate;
        granted.channels = channels;
        granted.format = f;
        granted.period_frames = req.period_frames;
        granted.buffer_frames = req.buffer_frames;
        FakeMicOptions o = po;
        o.rate = req.rate;
        pcm = std::make_unique<FakeMicPcm>(o, channels, f);
        return 0;
    };
    if (id.empty() && device == "default") { /* the sound server route: whatever is asked, from the first extra card */
        if (opt_.cards.empty()) return -ENOENT;
        if (opt_.cards.front().dead) return -EIO;
        FakeMicOptions o = opt_;
        o.mode = opt_.cards.front().mode;
        return grant(o, req.channels, SampleFormat::S16_LE);
    }
    for (const FakeCard &k : opt_.cards) {
        if (k.id != id) continue;
        FakeMicOptions o = opt_;
        o.mode = k.mode;
        if (k.dead) return -EIO;
        if (is_plug(device)) return grant(o, req.channels, req.format);
        if (k.hw_busy) return -EBUSY;
        if (k.hw_denied) return -EINVAL;
        bool rate_ok = false, fmt_ok = false;
        for (std::uint32_t r : k.rates) rate_ok = rate_ok || r == req.rate;
        for (SampleFormat f : k.formats) fmt_ok = fmt_ok || f == req.format;
        if (!rate_ok || !fmt_ok || req.channels < k.min_channels || req.channels > k.max_channels) return -EINVAL;
        return grant(o, req.channels, req.format);
    }
    if (opt_.mode == "none" || id != opt_.card_id) return -ENOENT;
    if (opt_.mode == "busy") return -EBUSY;
    if (req.rate != opt_.rate || req.channels != opt_.channels || req.format != SampleFormat::S16_LE) return -EINVAL;
    return grant(opt_, req.channels, req.format);
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
