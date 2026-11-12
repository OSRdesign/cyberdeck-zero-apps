/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette: the sound-card seam. AlsaApi is everything the app needs from ALSA; real_alsa.cpp implements it through
 * dlopen("libasound.so.2") (no link-time dependency), tests/alsa/fake_alsa.* implements it for the PC tests.
 * Error values are negative errno numbers, as ALSA returns them, plus kNoAlsa when libasound cannot be loaded.
 */
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {

constexpr int kNoAlsa = -100000; /* libasound.so.2 is missing or lacks a symbol */

/* A readable text for a negative error value ("Device or resource busy", "no ALSA library", ...). */
std::string alsa_error_text(int err);

enum class SampleFormat { S16_LE, S24_3LE, S32_LE };
const char *format_name(SampleFormat f);
unsigned format_bytes(SampleFormat f); /* bytes per sample: 2, 3, 4 */

struct CardInfo {
    int index = -1;         /* the number of this boot: NEVER stored, NEVER used to find a card again */
    std::string id;         /* card id string ("Device"); unique among the cards present */
    std::string name;       /* short name ("USB PnP Sound Device") */
    std::string long_name;  /* long name ("C-Media ... at usb-3f980000.usb-1.2, full speed") */
    std::string usb_by_id;  /* "/dev/snd/by-id/usb-..." when present, else "" */
    std::string usb_id;     /* "vvvv:pppp" when a USB card, else "" (a cheap headset may report 0000:0000) */
    std::string usb_port;   /* USB port path such as "1-1.2" when known, else "" */
    bool capture = false;   /* has a capture PCM */
    bool playback = false;  /* has a playback PCM (filled like `capture`, from /dev/snd pcmC<n>D<d>p) */
};

/* What a capture PCM offers (from hw_params_any and test_*). */
struct CaptureOffer {
    std::vector<uint32_t> rates;           /* the common rates the device accepts exactly */
    unsigned min_channels = 0, max_channels = 0;
    std::vector<SampleFormat> formats;
    bool has_rate(uint32_t r) const;
    bool has_format(SampleFormat f) const;
};

struct HwRequest {
    uint32_t rate = 48000;
    unsigned channels = 1;
    SampleFormat format = SampleFormat::S16_LE;
    uint32_t period_frames = 1024;
    uint32_t buffer_frames = 24576;
};
struct HwGranted {
    uint32_t rate = 0;
    unsigned channels = 0;
    SampleFormat format = SampleFormat::S16_LE;
    uint32_t period_frames = 0;
    uint32_t buffer_frames = 0;
};

/* What a playback PCM offers: the same fields as a capture offer. */
using PlaybackOffer = CaptureOffer;

/* One open capture or playback PCM. Used by one thread at a time. */
class AlsaPcm {
public:
    virtual ~AlsaPcm() = default;
    /* Frames read (> 0, possibly fewer than asked) or a negative error (-EPIPE overrun, -ENODEV/-EBADFD unplugged...). */
    virtual long readi(void *buf, size_t frames) = 0;
    /* Playback PCMs: frames written (> 0, possibly fewer) or a negative error (-EPIPE underrun, -ENODEV unplugged...). */
    virtual long writei(const void *buf, size_t frames) {
        (void)buf;
        (void)frames;
        return -ENOSYS; /* a capture PCM */
    }
    /* Playback PCMs: frames queued but not yet heard (snd_pcm_delay), or a negative error. */
    virtual long delay() { return 0; }
    /* Playback PCMs: stop at once and discard what is queued (snd_pcm_drop) / play out what is queued (snd_pcm_drain). */
    virtual int drop() { return 0; }
    virtual int drain() { return 0; }
    /* snd_pcm_recover: 0 when the stream can be read again, else a negative error. */
    virtual int recover(int err) = 0;
    virtual int prepare() = 0;
    virtual void close() = 0; /* idempotent */
};

/* A mixer control of one card, by its control name ("Mic Capture Volume"). */
struct CtlInfo {
    enum class Type { Integer, Boolean, Other };
    std::string name;
    Type type = Type::Other;
    bool capture = false;  /* the name says Capture */
    bool readable = true, writable = true;
    int count = 1;         /* channels (values) */
    long min = 0, max = 0, step = 1;
    bool has_db = false;   /* the driver reports dB for min and max */
    long db_min_cdb = 0, db_max_cdb = 0; /* hundredths of a dB, when has_db */
};

class AlsaApi {
public:
    virtual ~AlsaApi() = default;
    /* false with a reason when the library cannot be used. Card listing may still work without it. */
    virtual bool available(std::string *why) = 0;
    virtual std::vector<CardInfo> cards() = 0;
    /* What the capture PCM offers; opens it briefly. -EBUSY when held by someone else. */
    virtual int query_capture(const std::string &device, CaptureOffer &out) = 0;
    /* Opens and configures (interleaved read/write, exactly the requested format and rate: no conversion). */
    virtual int open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                             HwGranted &granted) = 0;
    /* Mixer: `card` is "hw:CARD=<id>". */
    virtual int mixer_list(const std::string &card, std::vector<CtlInfo> &out) = 0;
    virtual int mixer_get(const std::string &card, const std::string &name, long &value) = 0;       /* first channel */
    virtual int mixer_set(const std::string &card, const std::string &name, long value) = 0;        /* all channels */

    /* Playback (task T5). Defaults return -ENOSYS so a capture-only implementation still builds. */
    /* What the playback PCM offers; opens it briefly. */
    virtual int query_playback(const std::string &device, PlaybackOffer &out) {
        (void)device;
        (void)out;
        return -ENOSYS;
    }
    /* Opens and configures a playback PCM: interleaved write, exactly the requested format, rate and channels. */
    virtual int open_playback(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                              HwGranted &granted) {
        (void)device;
        (void)req;
        (void)pcm;
        (void)granted;
        return -ENOSYS;
    }
};

/* The production implementation: loads libasound.so.2 on first use. Never null; unavailable() explains. */
std::unique_ptr<AlsaApi> make_real_alsa();

} // namespace vinyl
