/* SPDX-License-Identifier: MIT */
#include "open_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace cassette {

void convert_s32_to_s24_3(const void *in, void *out, size_t samples) {
    const uint8_t *s = static_cast<const uint8_t *>(in); /* little endian: byte 0 is the lowest */
    uint8_t *d = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < samples; ++i) {
        d[3 * i + 0] = s[4 * i + 1];
        d[3 * i + 1] = s[4 * i + 2];
        d[3 * i + 2] = s[4 * i + 3];
    }
}

bool sound_server_running(const std::function<bool(const std::string &)> &is_socket, const char *xdg_runtime_dir,
                          unsigned uid) {
    std::string dir = xdg_runtime_dir ? xdg_runtime_dir : "";
    if (dir.empty()) dir = "/run/user/" + std::to_string(uid);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return is_socket(dir + "/pipewire-0") || is_socket(dir + "/pulse/native");
}

bool sound_server_running_here() {
    return sound_server_running(
        [](const std::string &p) {
            struct stat st;
            return ::stat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        },
        std::getenv("XDG_RUNTIME_DIR"), static_cast<unsigned>(::getuid()));
}

namespace {

uint32_t buffer_frames_for(uint32_t rate, const OpenOptions &opt) {
    uint64_t want = (static_cast<uint64_t>(rate) * opt.min_buffer_ms + 999) / 1000;
    uint32_t p = std::max<uint32_t>(opt.period_frames, 1);
    return static_cast<uint32_t>(((want + p - 1) / p) * p); /* whole periods */
}

void fail(OpenResult &r, OpenStatus st, int err, const std::string &msg) {
    r.status = st;
    r.err = err;
    r.message = msg;
}

OpenStatus status_for(int err) {
    if (err == kNoAlsa) return OpenStatus::NoAlsa;
    if (err == -EBUSY) return OpenStatus::Busy;
    if (err == -ENODEV || err == -ENOENT || err == -EBADFD) return OpenStatus::NoCard;
    return OpenStatus::Failed;
}

} // namespace

uint32_t choose_rate(const CaptureOffer &offer) {
    if (offer.has_rate(48000)) return 48000;
    if (offer.has_rate(44100)) return 44100;
    uint32_t best_below = 0, lowest = 0;
    for (uint32_t r : offer.rates) {
        if (r == 0) continue;
        if (r <= 48000 && r > best_below) best_below = r;
        if (lowest == 0 || r < lowest) lowest = r;
    }
    return best_below ? best_below : lowest; /* lowest: every offered rate is above 48000 */
}

bool choose_params(const CaptureOffer &offer, const OpenOptions &opt, HwRequest &req, Conversion &conv, AudioFormat &fmt,
                   std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    const uint32_t rate = opt.want_rate && offer.has_rate(opt.want_rate) ? opt.want_rate : choose_rate(offer);
    if (rate == 0) return no("the card offers no sample rate");
    req.rate = rate;

    if (offer.max_channels == 0 || offer.min_channels > 2) return no("the card offers no mono or stereo capture");
    req.channels = std::min<unsigned>(offer.max_channels, 2);
    if (req.channels < offer.min_channels) req.channels = offer.min_channels;

    conv = Conversion::None;
    if (opt.want_bits == 24 && offer.has_format(SampleFormat::S24_3LE)) {
        req.format = SampleFormat::S24_3LE;
        fmt.bits = 24;
    } else if (opt.want_bits == 24 && offer.has_format(SampleFormat::S32_LE)) {
        req.format = SampleFormat::S32_LE;
        conv = Conversion::S32_to_S24_3;
        fmt.bits = 24;
    } else if (offer.has_format(SampleFormat::S16_LE)) {
        req.format = SampleFormat::S16_LE;
        fmt.bits = 16;
    } else if (offer.has_format(SampleFormat::S24_3LE)) {
        req.format = SampleFormat::S24_3LE;
        fmt.bits = 24;
    } else if (offer.has_format(SampleFormat::S32_LE)) {
        req.format = SampleFormat::S32_LE;
        conv = Conversion::S32_to_S24_3;
        fmt.bits = 24;
    } else {
        return no("the card offers no 16-bit or 24-bit capture format");
    }
    req.period_frames = opt.period_frames;
    req.buffer_frames = buffer_frames_for(req.rate, opt);
    fmt.rate = req.rate;
    fmt.channels = static_cast<uint16_t>(req.channels);
    return true;
}

OpenResult open_capture(AlsaApi &api, const CardInfo &card, const OpenOptions &opt_in) {
    OpenOptions opt = opt_in;
    OpenResult r;
    r.device = pcm_device(card);
    std::string why;
    if (!api.available(&why)) {
        fail(r, OpenStatus::NoAlsa, kNoAlsa, "No ALSA sound library on this system");
        return r;
    }

    int err = api.query_capture(r.device, r.offer);
    bool busy = err == -EBUSY;
    if (err == 0) {
        HwRequest req;
        std::string reason;
        if (opt.prefer) opt.prefer(r.offer, opt.want_rate, opt.want_bits);
        if (!choose_params(r.offer, opt, req, r.conv, r.format, &reason)) {
            fail(r, OpenStatus::Unsupported, -EINVAL, "Unsupported card: " + reason);
            return r;
        }
        err = api.open_capture(r.device, req, r.pcm, r.hw);
        busy = err == -EBUSY;
        if (err == 0) {
            r.status = OpenStatus::Ok;
            return r;
        }
    }
    if (!busy) {
        fail(r, status_for(err), err, "Cannot open the card: " + alsa_error_text(err));
        return r;
    }

    /* busy: the sound server may hold the card; record through its ALSA route */
    if (!opt.allow_server) {
        fail(r, OpenStatus::Busy, -EBUSY, "The card is in use by another program");
        return r;
    }
    HwRequest req;
    req.rate = 48000;
    req.channels = opt.server_hint && opt.server_hint->max_channels >= 2 && opt.server_hint->min_channels <= 2 ? 2 : 1;
    req.format = SampleFormat::S16_LE; /* the server converts to whatever we ask: S16 at 48000 */
    req.period_frames = opt.period_frames;
    req.buffer_frames = buffer_frames_for(req.rate, opt);
    r.device = opt.server_device;
    r.conv = Conversion::None;
    r.format.rate = req.rate;
    r.format.channels = static_cast<uint16_t>(req.channels);
    r.format.bits = 16;
    err = api.open_capture(r.device, req, r.pcm, r.hw);
    if (err != 0) {
        fail(r, status_for(err), err, "The card is in use and the sound server did not open: " + alsa_error_text(err));
        if (r.status == OpenStatus::NoCard) r.status = OpenStatus::Busy;
        return r;
    }
    r.via_server = true;
    r.status = OpenStatus::Ok;
    r.message = "Recording through the sound server: the card is in use";
    return r;
}

} // namespace cassette
