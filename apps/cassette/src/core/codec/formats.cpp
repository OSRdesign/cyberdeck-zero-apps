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

std::string rate_text(uint32_t rate) { return rate_label(rate); }

uint64_t round_u64(double v) { return v <= 0 ? 0 : static_cast<uint64_t>(std::llround(v)); }

/* compatibility and size of one choice at a capture format `cap` (the model's single rule set) */
void evaluate(const FormatInputs &in, FormatChoice &c, const AudioFormat &cap, RecFormat format, int bitrate) {
    const bool known = in.state == CaptureState::Known;
    std::string why;
    bool ok = true;
    if (in.state == CaptureState::Unusable) {
        ok = false;
        why = in.capture_why.empty() ? "the card cannot record" : in.capture_why;
    } else if (format == RecFormat::Wav) {
        if (known && block_align(cap) == 0) ok = false, why = "the card's format cannot be stored";
    } else if (format == RecFormat::Flac) {
        if (known && !flac_format_ok(cap, &why)) ok = false;
        else if (!in.codecs.flac) ok = false, why = "FLAC not installed (libflac14)";
    } else {
        if (known && cap.channels > 2) ok = false, why = "Opus records at most 2 channels";
        else if (known && !opus_rate_ok(cap.rate)) ok = false, why = "Opus cannot record at " + rate_text(cap.rate);
        else if (known && !opus_format_ok(cap, &why)) ok = false;
        else if (known && in.speech_only && bitrate > 32000) ok = false, why = "Bluetooth call audio is speech only: Opus up to 32 kbps";
        else if (!in.codecs.opus) ok = false, why = "Opus not installed (libopusenc0)";
    }
    c.compatible = ok;
    if (!ok) c.why_not = why;

    if (known) {
        SizeEstimate &s = c.size;
        const uint64_t pcm = pcm_bytes_per_minute(cap);
        if (format == RecFormat::Wav) {
            s.kind = SizeKind::Exact;
            s.bytes_per_minute = s.low_per_minute = s.high_per_minute = pcm;
            s.header_bytes = kDataOffset;
        } else if (format == RecFormat::Flac) {
            s.kind = SizeKind::About;
            const unsigned bits = cap.bits;
            const bool observed = in.flac_ratio > 0 && bits == in.capture.bits;
            const double typical = observed ? in.flac_ratio : flac_ratio_for_bits(kFlacRatioSpeech16, bits);
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
            s.bytes_per_minute = s.low_per_minute = s.high_per_minute = opus_bytes_per_minute(bitrate);
            s.header_bytes = kOpusHeaderBytes;
        }
    }
}


}  // namespace

std::string rate_label(uint32_t rate) {
    char b[32];
    std::snprintf(b, sizeof b, "%g kHz", rate / 1000.0);
    return b;
}

std::string rates_label(const std::vector<uint32_t> &rates) {
    if (rates.empty()) return "no rate";
    std::string s;
    for (size_t i = 0; i < rates.size(); ++i) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", rates[i] / 1000.0);
        if (i) s += i + 1 == rates.size() ? " or " : ", ";
        s += b;
    }
    return s + " kHz";
}

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
        evaluate(in, c, in.capture, p.format, p.bitrate);
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
        in.rates = offer.rates;
        in.has16 = offer.has_format(SampleFormat::S16_LE);
        in.has24 = offer.has_format(SampleFormat::S24_3LE) || offer.has_format(SampleFormat::S32_LE);
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

/* ======================================================================================================================== */
/* the three parameters */

const int kOpusKbps[7] = {16, 24, 32, 48, 64, 96, 128};

bool opus_kbps_ok(int kbps) {
    for (int k : kOpusKbps)
        if (k == kbps) return true;
    return false;
}

int preset_kbps(const std::string &preset) {
    for (const Preset &p : kPresets)
        if (p.format == RecFormat::Opus && preset == p.preset) return p.bitrate / 1000;
    return 0;
}

std::string preset_of_kbps(int kbps) {
    for (const Preset &p : kPresets)
        if (p.format == RecFormat::Opus && p.bitrate / 1000 == kbps) return p.preset;
    return "";
}

bool kbps_is_speech(int kbps) { return kbps <= 32; }

RecStored stored_of(const Settings &s) {
    RecStored r;
    r.codec = s.format;
    r.preset = s.format_preset;
    r.rate = s.rate;
    r.quality = s.quality;
    return r;
}

std::vector<uint32_t> card_rates(const FormatInputs &in) {
    std::vector<uint32_t> r;
    if (in.state != CaptureState::Known) return r;
    r = in.rates;
    if (r.empty() && in.capture.rate) r.push_back(in.capture.rate);
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    r.erase(std::remove(r.begin(), r.end(), 0u), r.end());
    return r;
}

std::vector<unsigned> card_bits(const FormatInputs &in) {
    std::vector<unsigned> b;
    if (in.has16) b.push_back(16);
    if (in.has24) b.push_back(24);
    if (b.empty()) b.push_back(16);
    return b;
}

unsigned default_bits(const FormatInputs &in) { return in.has16 || !in.has24 ? 16 : 24; }

bool rate_legal(RecFormat codec, uint32_t rate, std::string *why) {
    if (codec == RecFormat::Opus && !opus_rate_ok(rate)) {
        if (why) *why = "Opus needs 8, 12, 16, 24 or 48 kHz";
        return false;
    }
    if (codec == RecFormat::Flac) {
        AudioFormat f;
        f.rate = rate;
        f.channels = 1;
        f.bits = 16;
        if (!flac_format_ok(f, why)) return false;
    }
    return true;
}

std::vector<uint32_t> codec_rates(const FormatInputs &in, RecFormat codec) {
    std::vector<uint32_t> out;
    for (uint32_t r : card_rates(in))
        if (rate_legal(codec, r)) out.push_back(r);
    return out;
}

namespace {
bool has_rate(const std::vector<uint32_t> &v, uint32_t r) { return std::find(v.begin(), v.end(), r) != v.end(); }
}  // namespace

bool codec_usable(const FormatInputs &in, RecFormat codec, std::string *why) {
    auto no = [&](const std::string &m) {
        if (why) *why = m;
        return false;
    };
    if (in.state == CaptureState::Unusable) return no(in.capture_why.empty() ? "the card cannot record" : in.capture_why);
    const bool known = in.state == CaptureState::Known;
    if (codec == RecFormat::Wav) {
        if (known && block_align(in.capture) == 0) return no("the card's format cannot be stored");
        return true;
    }
    if (known) {
        if (codec == RecFormat::Opus && in.capture.channels > 2) return no("Opus records at most 2 channels");
        if (codec_rates(in, codec).empty()) {
            if (codec == RecFormat::Opus)
                return no("Opus needs 8, 12, 16, 24 or 48 kHz; this card has " + rates_label(card_rates(in)));
            return no("FLAC cannot store the card's sample rate");
        }
        AudioFormat f = in.capture;
        f.rate = default_rate(in, codec);
        f.bits = default_bits(in);
        std::string w;
        if (codec == RecFormat::Flac && !flac_format_ok(f, &w)) return no(w);
        if (codec == RecFormat::Opus && !opus_format_ok(f, &w)) return no(w);
    }
    if (codec == RecFormat::Flac && !in.codecs.flac) return no("FLAC not installed (libflac14)");
    if (codec == RecFormat::Opus && !in.codecs.opus) return no("Opus not installed (libopusenc0)");
    return true;
}

uint32_t default_rate(const FormatInputs &in, RecFormat codec) {
    const std::vector<uint32_t> rs = codec_rates(in, codec);
    if (rs.empty()) return 0;
    if (codec == RecFormat::Opus) {
        for (uint32_t r : {48000u, 16000u, 24000u, 12000u, 8000u})
            if (has_rate(rs, r)) return r;
        return rs.back();
    }
    CaptureOffer o;
    o.rates = rs;
    return choose_rate(o);
}

uint32_t nearest_rate(const FormatInputs &in, RecFormat codec, uint32_t from) {
    const std::vector<uint32_t> rs = codec_rates(in, codec);
    if (rs.empty()) return 0;
    if (has_rate(rs, from)) return from;
    if (codec == RecFormat::Opus) {
        if (has_rate(rs, 48000)) return 48000;
        if (has_rate(rs, 16000)) return 16000;
    } else {
        return default_rate(in, codec);
    }
    uint32_t best = rs.front();
    for (uint32_t r : rs) {
        const uint32_t dr = r > from ? r - from : from - r, db = best > from ? best - from : from - best;
        if (dr < db || (dr == db && r > best)) best = r;
    }
    return best;
}

RecParams resolve_params(const FormatInputs &in, const RecStored &st) {
    RecParams p;
    p.codec = st.codec;
    const std::vector<uint32_t> rs = codec_rates(in, st.codec);
    p.rate = st.rate > 0 && has_rate(rs, static_cast<uint32_t>(st.rate)) ? static_cast<uint32_t>(st.rate)
                                                                         : default_rate(in, st.codec);
    p.bits = default_bits(in);
    if (st.codec == RecFormat::Opus) {
        p.kbps = opus_kbps_ok(st.quality) ? st.quality : preset_kbps(st.preset);
        if (p.kbps == 0) p.kbps = 32;
    } else if (st.quality == 16 || st.quality == 24) {
        for (unsigned b : card_bits(in))
            if (b == static_cast<unsigned>(st.quality)) p.bits = b;
    }
    return p;
}

FormatChoice choice_for(const FormatInputs &in, const RecParams &p) {
    FormatChoice c;
    c.key.format = p.codec;
    c.flac_level = p.codec == RecFormat::Flac ? 5 : 0;
    switch (p.codec) {
    case RecFormat::Wav: c.id = "wav", c.name = "WAV", c.detail = "uncompressed, plays everywhere"; break;
    case RecFormat::Flac: c.id = "flac", c.name = "FLAC", c.detail = "lossless, a third to half the size"; break;
    default: {
        c.name = "Opus";
        c.detail = "small files, speech and music";
        c.id = "opus-" + std::to_string(p.kbps);
        c.key.preset = preset_of_kbps(p.kbps);
        c.opus_bitrate_bps = p.kbps * 1000;
        c.opus_app = kbps_is_speech(p.kbps) ? OpusApplication::Speech : OpusApplication::Music;
        c.opus_force_mono = kbps_is_speech(p.kbps);
        break;
    }
    }
    AudioFormat cap = in.capture;
    cap.rate = p.rate;
    cap.bits = p.bits;
    evaluate(in, c, cap, p.codec, c.opus_bitrate_bps); /* the size is shown for a dimmed entry too */
    std::string why;
    if (!codec_usable(in, p.codec, &why)) {
        c.compatible = false;
        c.why_not = why;
    }
    return c;
}

RecParams effective_params(const FormatInputs &in, const RecStored &st, bool *fell_back) {
    const RecParams p = resolve_params(in, st);
    if (fell_back) *fell_back = false;
    if (st.codec == RecFormat::Wav || choice_for(in, p).compatible) return p;
    if (fell_back) *fell_back = true;
    RecStored wav;
    wav.codec = RecFormat::Wav;
    return resolve_params(in, wav);
}

FormatChoice effective_for(const FormatInputs &in, const RecStored &st, bool *fell_back) {
    return choice_for(in, effective_params(in, st, fell_back));
}

StoredChange change_codec(const FormatInputs &in, const RecStored &cur, RecFormat codec) {
    StoredChange r;
    r.stored = cur;
    if (codec == cur.codec) return r;
    const RecParams old = resolve_params(in, cur);
    r.stored.codec = codec;
    r.stored.preset.clear();
    if ((cur.codec == RecFormat::Opus) != (codec == RecFormat::Opus)) r.stored.quality = 0; /* kbps and bits do not mix */
    if (in.state != CaptureState::Known) return r;
    const std::vector<uint32_t> rs = codec_rates(in, codec);
    if (rs.empty()) return r;
    const uint32_t want = old.rate ? old.rate : static_cast<uint32_t>(cur.rate);
    if (want && !has_rate(rs, want)) {
        const uint32_t to = nearest_rate(in, codec, want);
        r.stored.rate = static_cast<int>(to);
        const char *name = codec == RecFormat::Opus ? "Opus" : codec == RecFormat::Flac ? "FLAC" : "WAV";
        r.message = "Sample rate set to " + rate_label(to) + ": " + name + " cannot record at " + rate_label(want);
    }
    return r;
}

StoredChange change_preset(const RecStored &cur, const FormatKey &preset) {
    (void)cur;
    StoredChange r;
    r.stored.codec = preset.format;
    r.stored.preset = preset.preset;
    return r;
}

StoredChange change_rate(const RecStored &cur, uint32_t rate) {
    StoredChange r;
    r.stored = cur;
    r.stored.rate = static_cast<int>(rate);
    return r;
}

StoredChange change_quality(const RecStored &cur, int quality) {
    StoredChange r;
    r.stored = cur;
    r.stored.quality = quality;
    if (cur.codec == RecFormat::Opus) r.stored.preset.clear(); /* a plain bit rate: no longer a preset's */
    return r;
}

}  // namespace cassette
