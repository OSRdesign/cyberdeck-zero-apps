/* SPDX-License-Identifier: MIT */
#include <cerrno>

#include "check.hpp"
#include "fake_alsa.hpp"
#include "open_policy.hpp"

using namespace cassette;

namespace {

bool any_plughw(const FakeAlsa &f) {
    for (const auto &d : f.opened)
        if (d.find("plughw") != std::string::npos) return true;
    for (const auto &d : f.queried)
        if (d.find("plughw") != std::string::npos) return true;
    return false;
}

void direct_open_and_rate_choice() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0), fake_uc02(1)};
    OpenResult r = open_capture(f, f.cards_[0].info);
    CHECK(r.ok());
    CHECK(!r.via_server);
    CHECK_EQ(r.device, "hw:CARD=Device,DEV=0");
    CHECK_EQ(r.format.rate, 48000u); /* 48 kHz first, though 44.1 is offered too */
    CHECK_EQ(r.format.channels, 1u);
    CHECK_EQ(r.format.bits, 16u);
    CHECK(r.conv == Conversion::None);
    CHECK_EQ(r.hw.period_frames, 1024u);
    CHECK(r.hw.buffer_frames >= 24000u); /* at least 500 ms */
    CHECK_EQ(r.hw.buffer_frames % 1024u, 0u);
    CHECK(!any_plughw(f));
    CHECK(f.opened.size() == 1 && f.opened[0] == "hw:CARD=Device,DEV=0");

    /* the headset: mono, 48000 only */
    OpenResult h = open_capture(f, f.cards_[1].info);
    CHECK(h.ok());
    CHECK_EQ(h.format.rate, 48000u);
    CHECK_EQ(h.format.channels, 1u);

    /* a card with only 44100 */
    FakeAlsa g;
    g.cards_ = {fake_pcm2902(0)};
    g.cards_[0].offer.rates = {44100};
    OpenResult o = open_capture(g, g.cards_[0].info);
    CHECK(o.ok());
    CHECK_EQ(o.format.rate, 44100u);
    CHECK_EQ(g.requests[0].rate, 44100u);
    CHECK(o.hw.buffer_frames >= 22050u);
}

void channels_and_formats() {
    CaptureOffer off;
    off.rates = {48000};
    off.formats = {SampleFormat::S16_LE};
    OpenOptions opt;
    HwRequest req;
    Conversion conv;
    AudioFormat fmt;
    std::string why;

    off.min_channels = 1;
    off.max_channels = 2;
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK_EQ(req.channels, 2u); /* follows the device: stereo when it offers it */
    off.min_channels = off.max_channels = 1;
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK_EQ(fmt.channels, 1u);
    off.min_channels = 2;
    off.max_channels = 8;
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK_EQ(req.channels, 2u);
    off.min_channels = 4;
    CHECK(!choose_params(off, opt, req, conv, fmt, &why));

    off.min_channels = off.max_channels = 1;
    off.formats = {SampleFormat::S24_3LE, SampleFormat::S32_LE};
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK(req.format == SampleFormat::S24_3LE); /* kept as it is: packed 24-bit */
    CHECK(conv == Conversion::None);
    CHECK_EQ(fmt.bits, 24u);
    off.formats = {SampleFormat::S32_LE};
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK(req.format == SampleFormat::S32_LE);
    CHECK(conv == Conversion::S32_to_S24_3);
    CHECK_EQ(fmt.bits, 24u);
    off.formats = {SampleFormat::S16_LE, SampleFormat::S32_LE};
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK(req.format == SampleFormat::S16_LE); /* native S16 wins */
    off.formats.clear();
    CHECK(!choose_params(off, opt, req, conv, fmt, &why));
    CHECK(!why.empty());

    off.formats = {SampleFormat::S16_LE};
    off.rates = {32000, 96000};
    CHECK(!choose_params(off, opt, req, conv, fmt, &why));

    /* the packer drops the low byte */
    const uint8_t in[8] = {0xAA, 0x01, 0x02, 0x03, 0xBB, 0x04, 0x05, 0x06};
    uint8_t out[6] = {0};
    convert_s32_to_s24_3(in, out, 2);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4 && out[4] == 5 && out[5] == 6);

    /* unsupported card through open_capture */
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    f.cards_[0].offer.rates = {32000};
    OpenResult r = open_capture(f, f.cards_[0].info);
    CHECK(r.status == OpenStatus::Unsupported);
    CHECK(f.opened.empty()); /* nothing opened, no server detour */
}

void busy_falls_back_to_server() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    f.cards_[0].busy = true;
    OpenResult r = open_capture(f, f.cards_[0].info);
    CHECK(r.ok());
    CHECK(r.via_server);
    CHECK_EQ(r.device, "default");
    CHECK(f.queried.size() == 1 && f.queried[0] == "hw:CARD=Device,DEV=0"); /* hw: was tried first */
    CHECK(f.opened.size() == 1 && f.opened[0] == "default");
    CHECK(!any_plughw(f));
    CHECK_EQ(r.format.rate, 48000u);
    CHECK_EQ(r.format.bits, 16u);
    CHECK(!r.message.empty());

    /* the server route is switched off, or fails too: Busy, with a sentence for the UI */
    OpenOptions off;
    off.allow_server = false;
    OpenResult b = open_capture(f, f.cards_[0].info, off);
    CHECK(b.status == OpenStatus::Busy);
    CHECK(!b.via_server);
    f.server_error = -ENOENT;
    OpenResult c = open_capture(f, f.cards_[0].info);
    CHECK(c.status == OpenStatus::Busy);
    CHECK(!c.message.empty());

    /* stereo hint for the server route */
    f.server_error = 0;
    CaptureOffer hint;
    hint.min_channels = 1;
    hint.max_channels = 2;
    OpenOptions with_hint;
    with_hint.server_hint = &hint;
    OpenResult d = open_capture(f, f.cards_[0].info, with_hint);
    CHECK(d.ok());
    CHECK_EQ(d.format.channels, 2u);
}

void open_succeeds_after_a_race() {
    /* the query succeeds but the open itself is busy (someone took the card in between): same fallback */
    struct Racy : FakeAlsa {
        int open_capture(const std::string &dev, const HwRequest &q, std::unique_ptr<AlsaPcm> &p, HwGranted &g) override {
            if (dev.compare(0, 3, "hw:") == 0) {
                opened.push_back(dev);
                return -EBUSY;
            }
            return FakeAlsa::open_capture(dev, q, p, g);
        }
    } f;
    f.cards_ = {fake_pcm2902(0)};
    OpenResult r = open_capture(f, f.cards_[0].info);
    CHECK(r.ok());
    CHECK(r.via_server);
    CHECK_EQ(f.opened.size(), 2u);
    CHECK_EQ(f.opened[0], "hw:CARD=Device,DEV=0");
}

void other_failures() {
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    f.lib_available = false;
    OpenResult r = open_capture(f, f.cards_[0].info);
    CHECK(r.status == OpenStatus::NoAlsa);
    CHECK_EQ(r.err, kNoAlsa);
    CHECK(!r.message.empty());
    CHECK(f.opened.empty());

    f.lib_available = true;
    f.cards_[0].open_error = -ENODEV; /* the card vanished: no server detour */
    r = open_capture(f, f.cards_[0].info);
    CHECK(r.status == OpenStatus::NoCard);
    CHECK(f.opened.empty());

    f.cards_[0].open_error = -EIO;
    r = open_capture(f, f.cards_[0].info);
    CHECK(r.status == OpenStatus::Failed);
    CHECK(!r.via_server);

    CHECK_EQ(alsa_error_text(kNoAlsa), "no ALSA library (libasound.so.2 not found)");
    CHECK(!alsa_error_text(-EBUSY).empty());
}

} // namespace

void test_open() {
    direct_open_and_rate_choice();
    channels_and_formats();
    busy_falls_back_to_server();
    open_succeeds_after_a_race();
    other_failures();
}
