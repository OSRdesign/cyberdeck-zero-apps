/* SPDX-License-Identifier: MIT */
#include "formats.hpp"

#include "../takes/recordings.hpp"
#include "../wav.hpp"
#include "flac_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cassette {

namespace {

struct Preset {
    const char *id;
    RecFormat format;
    const char *preset;
    const char *name;
    const char *detail;
    int bitrate;
    OpusApplication app;
    bool force_mono;
};

/* The stable order of the Format row. */
const Preset kPresets[] = {
    {"wav", RecFormat::Wav, "", "WAV", "uncompressed, plays everywhere", 0, OpusApplication::Speech, false},
    {"flac", RecFormat::Flac, "", "FLAC", "lossless, a third to half the size", 0, OpusApplication::Music, false},
    {"opus-speech24", RecFormat::Opus, "speech24", "Opus 24 kbps", "speech, smallest", 24000, OpusApplication::Speech,
     true},
    {"opus-speech32", RecFormat::Opus, "speech32", "Opus 32 kbps", "speech, clear", 32000, OpusApplication::Speech,
     true},
    {"opus-music64", RecFormat::Opus, "music64", "Opus 64 kbps", "music and voice", 64000, OpusApplication::Music, false},
    {"opus-music128", RecFormat::Opus, "music128", "Opus 128 kbps", "music, best", 128000, OpusApplication::Music,
     false},
};
constexpr const char *kOpusDefaultPreset = "speech32";

std::string rate_text(uint32_t rate) {
    char b[32];
    if (rate % 1000 == 0) std::snprintf(b, sizeof b, "%u kHz", static_cast<unsigned>(rate / 1000));
    else std::snprintf(b, sizeof b, "%.1f kHz", rate / 1000.0);
    return b;
}

uint64_t round_u64(double v) { return v <= 0 ? 0 : static_cast<uint64_t>(std::llround(v)); }

}  // namespace

uint64_t SizeEstimate::bytes_for_seconds(double seconds) const {
    if (kind == SizeKind::Unknown || seconds <= 0) return 0;
    return header_bytes + round_u64(static_cast<double>(bytes_per_minute) * seconds / 60.0);
}

double flac_ratio_for_bits(double ratio16, unsigned bits) {
    if (bits <= 16) return ratio16;
    /* the extra low bits of a 24-bit card are noise (the analogue noise floor sits far above -144 dBFS): FLAC stores them
     * nearly verbatim */
    const double extra = static_cast<double>(bits) - 16.0;
    return (ratio16 * 16.0 + extra) / static_cast<double>(bits);
}

uint64_t pcm_bytes_per_minute(const AudioFormat &fmt) { return bytes_per_hour(fmt) / 60u; }

uint64_t opus_bytes_per_minute(int bitrate_bps) {
    if (bitrate_bps <= 0) return 0;
    /* 20 ms packets: 50 per second; each needs ceil((size + 1) / 255) lacing bytes; a page (27-byte header) at least
     * every second (the muxing delay) or every 255 lacing values */
    const double packet = bitrate_bps / 8.0 / 50.0;
    const double lacing_per_packet = std::floor(packet / 255.0) + 1.0;
    const double lacing_per_s = 50.0 * lacing_per_packet;
    const double pages_per_s = std::max(1.0, std::ceil(lacing_per_s / 255.0));
    const double per_s = bitrate_bps / 8.0 + lacing_per_s + 27.0 * pages_per_s;
    return round_u64(per_s * 60.0);
}

std::vector<FormatChoice> list_formats(const FormatInputs &in) {
    std::vector<FormatChoice> out;
    const bool known = in.state == CaptureState::Known;
    for (const Preset &p : kPresets) {
        FormatChoice c;
        c.id = p.id;
        c.key.format = p.format;
        c.key.preset = p.preset;
        c.name = p.name;
        c.detail = p.detail;
        c.opus_bitrate_bps = p.bitrate;
        c.opus_app = p.app;
        c.opus_force_mono = p.force_mono;
        c.flac_level = p.format == RecFormat::Flac ? 5 : 0;

        /* compatibility: the card's format first (installing a library would not help), then the library */
        std::string why;
        bool ok = true;
        if (in.state == CaptureState::Unusable) {
            ok = false;
            why = in.capture_why.empty() ? "the card cannot record" : in.capture_why;
        } else if (p.format == RecFormat::Wav) {
            if (known && block_align(in.capture) == 0) ok = false, why = "the card's format cannot be stored";
        } else if (p.format == RecFormat::Flac) {
            if (known && !flac_format_ok(in.capture, &why)) ok = false;
            else if (!in.codecs.flac) ok = false, why = "FLAC not installed (libflac14)";
        } else {
            if (known && in.capture.channels > 2) ok = false, why = "Opus records at most 2 channels";
            else if (known && !opus_rate_ok(in.capture.rate))
                ok = false, why = "Opus cannot record at " + rate_text(in.capture.rate);
            else if (known && !opus_format_ok(in.capture, &why)) ok = false;
            else if (!in.codecs.opus) ok = false, why = "Opus not installed (libopusenc0)";
        }
        c.compatible = ok;
        if (!ok) c.why_not = why;

        if (known) {
            SizeEstimate &s = c.size;
            const uint64_t pcm = pcm_bytes_per_minute(in.capture);
            if (p.format == RecFormat::Wav) {
                s.kind = SizeKind::Exact;
                s.bytes_per_minute = s.low_per_minute = s.high_per_minute = pcm;
                s.header_bytes = kDataOffset;
            } else if (p.format == RecFormat::Flac) {
                s.kind = SizeKind::About;
                const unsigned bits = in.capture.bits;
                const double typical = in.flac_ratio > 0 ? in.flac_ratio : flac_ratio_for_bits(kFlacRatioSpeech16, bits);
                double lo = flac_ratio_for_bits(kFlacRatioSpeech16, bits);
                double hi = flac_ratio_for_bits(kFlacRatioMusic16, bits);
                if (typical < lo) lo = typical;
                if (typical > hi) hi = typical;
                s.bytes_per_minute = round_u64(static_cast<double>(pcm) * typical);
                s.low_per_minute = round_u64(static_cast<double>(pcm) * lo);
                s.high_per_minute = round_u64(static_cast<double>(pcm) * hi);
                s.header_bytes = kFlacHeaderBytes;
            } else {
                s.kind = SizeKind::Target;
                s.bytes_per_minute = s.low_per_minute = s.high_per_minute = opus_bytes_per_minute(p.bitrate);
                s.header_bytes = kOpusHeaderBytes;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

FormatInputs inputs_for_offer(const CaptureOffer &offer, const CodecAvailability &codecs) {
    FormatInputs in;
    in.codecs = codecs;
    HwRequest req;
    Conversion conv = Conversion::None;
    AudioFormat fmt;
    std::string why;
    if (choose_params(offer, OpenOptions(), req, conv, fmt, &why)) {
        in.state = CaptureState::Known;
        in.capture = fmt;
    } else {
        in.state = CaptureState::Unusable;
        in.capture_why = why;
    }
    return in;
}

const FormatChoice *find_choice(const std::vector<FormatChoice> &list, const FormatKey &key) {
    const FormatChoice *fallback = nullptr;
    for (const FormatChoice &c : list) {
        if (c.key.format != key.format) continue;
        if (c.key.preset == key.preset) return &c;
        const bool is_default = key.format == RecFormat::Opus ? c.key.preset == kOpusDefaultPreset : c.key.preset.empty();
        if (is_default && !fallback) fallback = &c;
    }
    return fallback;
}

const FormatChoice *effective_choice(const std::vector<FormatChoice> &list, const FormatKey &stored) {
    const FormatChoice *c = find_choice(list, stored);
    if (c && c->compatible) return c;
    const FormatChoice *wav = find_choice(list, FormatKey{RecFormat::Wav, ""});
    return wav && wav->compatible ? wav : nullptr;
}

double hours_for_space(uint64_t free_bytes, const SizeEstimate &size) {
    if (size.kind == SizeKind::Unknown || size.bytes_per_minute == 0) return 0.0;
    return static_cast<double>(free_bytes) / (static_cast<double>(size.bytes_per_minute) * 60.0);
}

}  // namespace cassette
