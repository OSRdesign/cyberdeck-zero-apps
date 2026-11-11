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

void rate_choice_table() {
    struct Row { std::vector<uint32_t> offered; uint32_t want; };
    const Row rows[] = {
        {{44100, 48000}, 48000},        /* 48000 first */
        {{44100}, 44100},               /* then 44100 */
        {{16000}, 16000},               /* a speech headset */
        {{8000, 16000}, 16000},         /* the highest at or below 48000 */
        {{22050, 32000}, 32000},
        {{32000, 96000}, 32000},        /* below beats above */
        {{16000, 44100, 96000}, 44100},
        {{96000}, 96000},               /* only rates above 48000: the lowest */
        {{192000, 88200, 96000}, 88200},
        {{}, 0},
    };
    for (const Row &r : rows) {
        CaptureOffer o;
        o.rates = r.offered;
        CHECK_EQ(choose_rate(o), r.want);
    }

    /* the fake cards through open_capture, and the hardware request carries the chosen rate */
    FakeAlsa f;
    f.cards_ = {fake_headset16(0), fake_stereo22(1), fake_card96(2)};
    OpenResult h = open_capture(f, f.cards_[0].info);
    CHECK(h.ok());
    CHECK_EQ(h.format.rate, 16000u);
    CHECK_EQ(h.format.channels, 1u);
    CHECK_EQ(h.format.bits, 16u);
    CHECK_EQ(f.requests[0].rate, 16000u);
    CHECK_EQ(h.hw.buffer_frames % 1024u, 0u);
    CHECK(h.hw.buffer_frames >= 8000u && h.hw.buffer_frames < 8000u + 1024u); /* 500 ms at 16000 Hz */
    OpenResult s2 = open_capture(f, f.cards_[1].info);
    CHECK(s2.ok());
    CHECK_EQ(s2.format.rate, 32000u);
    CHECK_EQ(s2.format.channels, 2u);
    OpenResult n = open_capture(f, f.cards_[2].info);
    CHECK(n.ok());
    CHECK_EQ(n.format.rate, 96000u);
    CHECK(!any_plughw(f));

    /* the sound-server route still asks for S16 at 48000, whatever the card offers */
    FakeAlsa g;
    g.cards_ = {fake_headset16(0)};
    g.cards_[0].busy = true;
    OpenResult b = open_capture(g, g.cards_[0].info);
    CHECK(b.ok());
    CHECK(b.via_server);
    CHECK_EQ(b.format.rate, 48000u);
    CHECK_EQ(g.requests[0].rate, 48000u);
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
    off.rates = {32000, 96000}; /* a rate at or below 48000 beats the ones above */
    CHECK(choose_params(off, opt, req, conv, fmt, &why));
    CHECK_EQ(req.rate, 32000u);
    off.rates.clear(); /* no rate at all: the only refusal on rate */
    CHECK(!choose_params(off, opt, req, conv, fmt, &why));
    CHECK_EQ(why, "the card offers no sample rate");

    /* the packer drops the low byte */
    const uint8_t in[8] = {0xAA, 0x01, 0x02, 0x03, 0xBB, 0x04, 0x05, 0x06};
    uint8_t out[6] = {0};
    convert_s32_to_s24_3(in, out, 2);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4 && out[4] == 5 && out[5] == 6);

    /* unsupported card through open_capture (no rate offered) */
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    f.cards_[0].offer.rates = {};
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

void server_detection() {
    auto only = [](const std::string &sock) { return [sock](const std::string &p) { return p == sock; }; };
    CHECK(sound_server_running(only("/run/user/1000/pipewire-0"), "/run/user/1000", 1000));
    CHECK(sound_server_running(only("/run/user/1000/pulse/native"), "/run/user/1000", 1000));
    CHECK(sound_server_running(only("/run/user/1000/pulse/native"), "/run/user/1000/", 1000)); /* trailing slash */
    CHECK(!sound_server_running(only("/run/user/1000/other"), "/run/user/1000", 1000));
    CHECK(!sound_server_running([](const std::string &) { return false; }, "/run/user/1000", 1000)); /* an appliance board */
    /* no XDG_RUNTIME_DIR: /run/user/<uid> */
    CHECK(sound_server_running(only("/run/user/1001/pipewire-0"), nullptr, 1001));
    CHECK(sound_server_running(only("/run/user/1001/pipewire-0"), "", 1001));
    CHECK(!sound_server_running(only("/run/user/1000/pipewire-0"), nullptr, 1001));
    /* the real check answers without throwing (the result depends on the machine) */
    (void)sound_server_running_here();
}

/* The user's choice of rate and depth (Settings: Sample rate, Quality): a rate the card offers is used, else the default
 * order; 24 prefers S24_3LE, then S32_LE converted; `prefer` fills both once the card's offer is known. */
void chosen_rate_and_depth() {
    CaptureOffer o;
    o.rates = {16000, 44100, 48000, 96000};
    o.min_channels = o.max_channels = 1;
    o.formats = {SampleFormat::S16_LE, SampleFormat::S24_3LE, SampleFormat::S32_LE};
    HwRequest req;
    Conversion conv;
    AudioFormat fmt;
    std::string why;
    OpenOptions opt;
    CHECK(choose_params(o, opt, req, conv, fmt, &why)); /* no choice: as ever */
    CHECK(fmt.rate == 48000 && fmt.bits == 16 && conv == Conversion::None);
    opt.want_rate = 96000;
    opt.want_bits = 24;
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK(fmt.rate == 96000 && fmt.bits == 24 && req.format == SampleFormat::S24_3LE && conv == Conversion::None);
    CHECK_EQ(req.rate, 96000u);
    opt.want_rate = 22050; /* not offered: the default order */
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK_EQ(fmt.rate, 48000u);
    opt.want_rate = 16000;
    opt.want_bits = 16;
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK(fmt.rate == 16000 && fmt.bits == 16 && req.format == SampleFormat::S16_LE);
    CHECK_EQ(req.buffer_frames % 1024u, 0u);
    CHECK(req.buffer_frames >= 8000u); /* 500 ms at the chosen rate */
    /* S32 only: 24 means converted packed 24; a card without 24-bit keeps 16 */
    o.formats = {SampleFormat::S16_LE, SampleFormat::S32_LE};
    opt.want_bits = 24;
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK(fmt.bits == 24 && req.format == SampleFormat::S32_LE && conv == Conversion::S32_to_S24_3);
    o.formats = {SampleFormat::S16_LE};
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK(fmt.bits == 16 && req.format == SampleFormat::S16_LE);
    /* a 24-bit-only card asked for 16: its own 24 */
    o.formats = {SampleFormat::S24_3LE};
    opt.want_bits = 16;
    CHECK(choose_params(o, opt, req, conv, fmt, &why));
    CHECK_EQ(fmt.bits, 24u);

    /* through open_capture: the callback decides from the offer, and the hardware request carries it */
    FakeAlsa f;
    f.cards_ = {fake_pcm2902(0)};
    f.cards_[0].offer.rates = {16000, 44100, 48000};
    f.cards_[0].offer.formats = {SampleFormat::S16_LE, SampleFormat::S24_3LE};
    OpenOptions po;
    unsigned seen_rates = 0;
    po.prefer = [&seen_rates](const CaptureOffer &offer, uint32_t &rate, unsigned &bits) {
        seen_rates = static_cast<unsigned>(offer.rates.size());
        rate = 44100;
        bits = 24;
    };
    OpenResult r = open_capture(f, f.cards_[0].info, po);
    CHECK(r.ok());
    CHECK_EQ(seen_rates, 3u);
    CHECK(r.format.rate == 44100 && r.format.bits == 24 && r.hw.rate == 44100 && r.hw.format == SampleFormat::S24_3LE);
    CHECK(f.requests.back().rate == 44100 && f.requests.back().format == SampleFormat::S24_3LE);
    CHECK_EQ(r.offer.rates.size(), size_t(3)); /* the offer comes back for the model */
    /* a preference of 0 (no choice): the default */
    OpenOptions zero;
    zero.prefer = [](const CaptureOffer &, uint32_t &rate, unsigned &bits) { rate = 0, bits = 0; };
    r = open_capture(f, f.cards_[0].info, zero);
    CHECK(r.ok() && r.format.rate == 48000 && r.format.bits == 16);
    /* the busy card routed through the sound server ignores the choice (48 kHz S16, the server converts) */
    FakeAlsa g;
    g.cards_ = {fake_pcm2902(0)};
    g.cards_[0].busy = true;
    OpenOptions bo;
    bo.want_rate = 16000;
    bo.want_bits = 24;
    OpenResult b = open_capture(g, g.cards_[0].info, bo);
    CHECK(b.ok() && b.via_server && b.format.rate == 48000 && b.format.bits == 16);
}

void test_open() {
    server_detection();
    direct_open_and_rate_choice();
    rate_choice_table();
    chosen_rate_and_depth();
    channels_and_formats();
    busy_falls_back_to_server();
    open_succeeds_after_a_race();
    other_failures();
}
