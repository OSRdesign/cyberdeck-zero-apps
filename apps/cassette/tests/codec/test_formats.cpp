/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: the format model for every kind of card (mono 48 kHz S16 headset, stereo 44.1 kHz, a 16 kHz-only
 * speech headset, a 24-bit card, an 8-channel card), libraries present or missing, the size model, the stored keys, and
 * the settings file's new "format_preset" key; then the three-parameter model (codec, sample rate, quality): every legal
 * combination per codec, the dimming rules, the 44.1 kHz-only card, the re-validation on a codec change, old settings
 * files and the "rate" and "quality" keys, the size estimates.
 */
#include "check.hpp"

#include "fileops.hpp"
#include "formats.hpp"
#include "takes/recordings.hpp"
#include "takes/settings.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace cassette;

namespace {

CodecAvailability both() {
    CodecAvailability a;
    a.flac = a.opus = true;
    return a;
}

CaptureOffer offer(std::vector<uint32_t> rates, unsigned min_ch, unsigned max_ch, std::vector<SampleFormat> fmts) {
    CaptureOffer o;
    o.rates = std::move(rates);
    o.min_channels = min_ch;
    o.max_channels = max_ch;
    o.formats = std::move(fmts);
    return o;
}

const FormatChoice &by_id(const std::vector<FormatChoice> &l, const std::string &id) {
    for (const FormatChoice &c : l)
        if (c.id == id) return c;
    static FormatChoice none;
    std::fprintf(stderr, "no format %s\n", id.c_str());
    ++g_fail;
    return none;
}

void test_order_and_keys() {
    const std::vector<FormatChoice> l = list_formats(inputs_for_offer(offer({48000}, 1, 1, {SampleFormat::S16_LE}), both()));
    const char *ids[] = {"wav", "flac", "opus-speech24", "opus-speech32", "opus-music64", "opus-music128"};
    CHECK_EQ(l.size(), size_t(6));
    for (size_t i = 0; i < 6 && i < l.size(); ++i) CHECK_EQ(l[i].id, std::string(ids[i]));
    CHECK(l[0].key == (FormatKey{RecFormat::Wav, ""}));
    CHECK(l[1].key == (FormatKey{RecFormat::Flac, ""}));
    CHECK(l[2].key == (FormatKey{RecFormat::Opus, "speech24"}));
    CHECK(l[5].key == (FormatKey{RecFormat::Opus, "music128"}));
    CHECK_EQ(l[1].flac_level, 5u);
    CHECK_EQ(l[2].opus_bitrate_bps, 24000);
    CHECK(l[2].opus_force_mono && l[3].opus_force_mono && !l[4].opus_force_mono);
    CHECK(l[2].opus_app == OpusApplication::Speech && l[5].opus_app == OpusApplication::Music);
    for (const FormatChoice &c : l) {
        CHECK(!c.name.empty());
        CHECK(!c.detail.empty());
    }
    /* stored keys: an empty or unknown preset is the format's default */
    CHECK_EQ(find_choice(l, FormatKey{RecFormat::Opus, ""})->id, std::string("opus-speech32"));
    CHECK_EQ(find_choice(l, FormatKey{RecFormat::Opus, "gibberish"})->id, std::string("opus-speech32"));
    CHECK_EQ(find_choice(l, FormatKey{RecFormat::Opus, "music64"})->id, std::string("opus-music64"));
    CHECK_EQ(find_choice(l, FormatKey{RecFormat::Flac, "level8"})->id, std::string("flac"));
    CHECK_EQ(find_choice(l, FormatKey{RecFormat::Wav, ""})->id, std::string("wav"));
}

void test_mono48_headset() {
    /* the CM108 / UC02 headset: mono S16, 44.1 and 48 kHz offered, 48 kHz chosen */
    const FormatInputs in = inputs_for_offer(offer({44100, 48000}, 1, 1, {SampleFormat::S16_LE}), both());
    CHECK(in.state == CaptureState::Known);
    CHECK_EQ(in.capture.rate, 48000u);
    CHECK_EQ(in.capture.channels, uint16_t(1));
    const std::vector<FormatChoice> l = list_formats(in);
    for (const FormatChoice &c : l) CHECK(c.compatible);
    const FormatChoice &wav = by_id(l, "wav");
    CHECK(wav.size.kind == SizeKind::Exact);
    CHECK_EQ(wav.size.bytes_per_minute, uint64_t(5760000)); /* 48000 x 1 x 2 x 60 */
    CHECK_EQ(wav.size.header_bytes, uint64_t(5120));
    CHECK_EQ(wav.size.bytes_for_seconds(60), uint64_t(5760000 + 5120));
    const FormatChoice &flac = by_id(l, "flac");
    CHECK(flac.size.kind == SizeKind::About);
    CHECK_EQ(flac.size.bytes_per_minute, uint64_t(5760000 * kFlacRatioSpeech16 + 0.5));
    CHECK_EQ(flac.size.low_per_minute, flac.size.bytes_per_minute);
    CHECK_EQ(flac.size.high_per_minute, uint64_t(5760000 * kFlacRatioMusic16 + 0.5));
    const FormatChoice &o24 = by_id(l, "opus-speech24");
    CHECK(o24.size.kind == SizeKind::Target);
    /* 3000 B/s of bitrate + 50 lacing bytes + one 27-byte page header a second */
    CHECK_EQ(o24.size.bytes_per_minute, uint64_t((3000 + 50 + 27) * 60));
    /* 128 kbps: 320-byte packets need 2 lacing values: 16000 + 100 + 27 */
    CHECK_EQ(by_id(l, "opus-music128").size.bytes_per_minute, uint64_t((16000 + 100 + 27) * 60));
    CHECK(by_id(l, "opus-speech32").size.bytes_per_minute < by_id(l, "opus-music64").size.bytes_per_minute);
    /* time left: WAV equals the recorder's own hours_left */
    const uint64_t free_b = 8000000000ull;
    CHECK(std::fabs(hours_for_space(free_b, wav.size) - hours_left(free_b, in.capture)) < 1e-9);
    CHECK(hours_for_space(free_b, o24.size) > 700.0); /* about 722 h of 24 kbps Opus in 8 GB */
    CHECK(hours_for_space(free_b, flac.size) > hours_for_space(free_b, wav.size) * 3.0);
    /* the study's figure: 8 GB holds 23 h of 48 kHz mono WAV */
    CHECK(hours_for_space(free_b, wav.size) > 23.0 && hours_for_space(free_b, wav.size) < 23.2);
}

void test_stereo44() {
    /* a stereo card at 44.1 kHz only: no Opus in 0.1 (no resampling, D11) */
    const FormatInputs in = inputs_for_offer(offer({44100}, 2, 2, {SampleFormat::S16_LE}), both());
    CHECK_EQ(in.capture.rate, 44100u);
    CHECK_EQ(in.capture.channels, uint16_t(2));
    const std::vector<FormatChoice> l = list_formats(in);
    CHECK(by_id(l, "wav").compatible);
    CHECK(by_id(l, "flac").compatible);
    for (const char *id : {"opus-speech24", "opus-speech32", "opus-music64", "opus-music128"}) {
        CHECK(!by_id(l, id).compatible);
        CHECK_EQ(by_id(l, id).why_not, std::string("Opus cannot record at 44.1 kHz"));
        CHECK(by_id(l, id).size.kind == SizeKind::Target); /* a figure is still there, for comparison */
    }
    CHECK_EQ(by_id(l, "wav").size.bytes_per_minute, uint64_t(44100 * 2 * 2 * 60));
    /* the stored Opus choice falls back to WAV on this card */
    CHECK_EQ(effective_choice(l, FormatKey{RecFormat::Opus, "music128"})->id, std::string("wav"));
    CHECK_EQ(effective_choice(l, FormatKey{RecFormat::Flac, ""})->id, std::string("flac"));
}

void test_16k_speech_headset() {
    /* a Jabra-style speech headset: 16 kHz mono only */
    const FormatInputs in = inputs_for_offer(offer({16000}, 1, 1, {SampleFormat::S16_LE}), both());
    CHECK_EQ(in.capture.rate, 16000u);
    const std::vector<FormatChoice> l = list_formats(in);
    for (const FormatChoice &c : l) CHECK(c.compatible);
    CHECK_EQ(by_id(l, "wav").size.bytes_per_minute, uint64_t(16000 * 2 * 60));
    CHECK_EQ(by_id(l, "flac").size.bytes_per_minute, uint64_t(1920000 * kFlacRatioSpeech16 + 0.5));
    CHECK_EQ(effective_choice(l, FormatKey{RecFormat::Opus, "speech24"})->id, std::string("opus-speech24"));
}

void test_24bit_card() {
    const FormatInputs in = inputs_for_offer(offer({48000, 96000}, 2, 2, {SampleFormat::S24_3LE}), both());
    CHECK_EQ(in.capture.bits, uint16_t(24));
    CHECK_EQ(in.capture.rate, 48000u);
    const std::vector<FormatChoice> l = list_formats(in);
    for (const FormatChoice &c : l) CHECK(c.compatible);
    CHECK_EQ(by_id(l, "wav").size.bytes_per_minute, uint64_t(48000 * 2 * 3 * 60));
    /* 24 bits: (0.30 x 16 + 8) / 24 = 0.5333 of PCM */
    const double r24 = flac_ratio_for_bits(kFlacRatioSpeech16, 24);
    CHECK(std::fabs(r24 - (kFlacRatioSpeech16 * 16 + 8) / 24) < 1e-12);
    CHECK_EQ(by_id(l, "flac").size.bytes_per_minute, uint64_t(17280000 * r24 + 0.5));
    /* Opus sizes do not depend on the bit depth */
    CHECK_EQ(by_id(l, "opus-music64").size.bytes_per_minute, opus_bytes_per_minute(64000));
    /* an S32 card is recorded as 24-bit packed: the same list */
    const FormatInputs s32 = inputs_for_offer(offer({48000}, 2, 2, {SampleFormat::S32_LE}), both());
    CHECK_EQ(s32.capture.bits, uint16_t(24));
    CHECK_EQ(list_formats(s32)[0].size.bytes_per_minute, uint64_t(17280000));
}

void test_8channel_card() {
    /* offers 8 channels only: the open policy records mono or stereo, so this card cannot record at all */
    const FormatInputs in = inputs_for_offer(offer({48000}, 8, 8, {SampleFormat::S16_LE}), both());
    CHECK(in.state == CaptureState::Unusable);
    const std::vector<FormatChoice> l = list_formats(in);
    for (const FormatChoice &c : l) {
        CHECK(!c.compatible);
        CHECK_EQ(c.why_not, std::string("the card offers no mono or stereo capture"));
        CHECK(c.size.kind == SizeKind::Unknown);
    }
    CHECK(effective_choice(l, FormatKey{RecFormat::Wav, ""}) == nullptr);
    /* 1..8 channels offered: stereo is chosen and everything works */
    const std::vector<FormatChoice> l2 = list_formats(inputs_for_offer(offer({48000}, 1, 8, {SampleFormat::S16_LE}), both()));
    for (const FormatChoice &c : l2) CHECK(c.compatible);
    CHECK_EQ(l2[0].size.bytes_per_minute, uint64_t(48000 * 2 * 2 * 60));
    /* a future policy that records all 8: WAV stores them, FLAC and Opus say why not */
    FormatInputs eight;
    eight.state = CaptureState::Known;
    eight.capture = AudioFormat{48000, 8, 16};
    eight.codecs = both();
    const std::vector<FormatChoice> l3 = list_formats(eight);
    CHECK(by_id(l3, "wav").compatible);
    CHECK_EQ(by_id(l3, "wav").size.bytes_per_minute, uint64_t(48000 * 8 * 2 * 60));
    CHECK(!by_id(l3, "flac").compatible);
    CHECK_EQ(by_id(l3, "flac").why_not, std::string("more than 2 channels"));
    CHECK(!by_id(l3, "opus-music128").compatible);
    CHECK_EQ(by_id(l3, "opus-music128").why_not, std::string("Opus records at most 2 channels"));
}

void test_libraries_missing() {
    CodecAvailability none;
    none.flac_why = "FLAC library not installed (libFLAC.so.12: cannot open shared object file)";
    none.opus_why = "Opus library not installed";
    const std::vector<FormatChoice> l = list_formats(inputs_for_offer(offer({48000}, 1, 1, {SampleFormat::S16_LE}), none));
    CHECK(by_id(l, "wav").compatible);
    CHECK(!by_id(l, "flac").compatible);
    CHECK_EQ(by_id(l, "flac").why_not, std::string("FLAC not installed (libflac14)"));
    CHECK(!by_id(l, "opus-speech32").compatible);
    CHECK_EQ(by_id(l, "opus-speech32").why_not, std::string("Opus not installed (libopusenc0)"));
    CHECK(by_id(l, "flac").size.kind == SizeKind::About); /* the figure stays, the entry is greyed */
    CHECK_EQ(effective_choice(l, FormatKey{RecFormat::Flac, ""})->id, std::string("wav"));
    /* only FLAC present */
    CodecAvailability flac_only;
    flac_only.flac = true;
    const std::vector<FormatChoice> l2 = list_formats(inputs_for_offer(offer({48000}, 1, 1, {SampleFormat::S16_LE}), flac_only));
    CHECK(by_id(l2, "flac").compatible);
    CHECK(!by_id(l2, "opus-speech24").compatible);
    /* the card's reason comes before the library's: on a 44.1 kHz card without libopusenc, the rate is the reason */
    const std::vector<FormatChoice> l3 = list_formats(inputs_for_offer(offer({44100}, 1, 1, {SampleFormat::S16_LE}), none));
    CHECK_EQ(by_id(l3, "opus-speech24").why_not, std::string("Opus cannot record at 44.1 kHz"));
    /* no card: compatibility by library only, no figures */
    FormatInputs nocard;
    nocard.codecs = flac_only;
    const std::vector<FormatChoice> l4 = list_formats(nocard);
    CHECK(by_id(l4, "wav").compatible);
    CHECK(by_id(l4, "flac").compatible);
    CHECK(!by_id(l4, "opus-music64").compatible);
    for (const FormatChoice &c : l4) CHECK(c.size.kind == SizeKind::Unknown && c.size.bytes_per_minute == 0);
    CHECK_EQ(hours_for_space(1000000000ull, l4[0].size), 0.0);
}

void test_observed_ratio() {
    FormatInputs in = inputs_for_offer(offer({48000}, 1, 1, {SampleFormat::S16_LE}), both());
    in.flac_ratio = 0.62; /* a music take ran at 62 % */
    const std::vector<FormatChoice> l = list_formats(in);
    const FormatChoice &flac = l[1];
    CHECK_EQ(flac.size.bytes_per_minute, uint64_t(5760000 * 0.62 + 0.5));
    CHECK_EQ(flac.size.high_per_minute, flac.size.bytes_per_minute); /* the range grows to include it */
    CHECK_EQ(flac.size.low_per_minute, uint64_t(5760000 * kFlacRatioSpeech16 + 0.5));
    /* the Opus overhead model */
    CHECK_EQ(opus_bytes_per_minute(0), uint64_t(0));
    CHECK_EQ(opus_bytes_per_minute(32000), uint64_t((4000 + 50 + 27) * 60));
}

std::string temp_dir() {
    std::string t = (std::filesystem::temp_directory_path() / "cassette-fmt-XXXXXX").string();
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    return mkdtemp(b.data()) ? std::string(b.data()) : std::string("/tmp");
}

std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_settings_key() {
    const std::string dir = temp_dir();
    const std::string path = dir + "/settings.json";
    FileOps &ops = posix_file_ops();
    {
        /* an old file: "format" only. Loads with an empty preset and keeps it absent on save */
        std::ofstream(path) << "{\"version\":1,\"format\":\"opus\",\"future_key\":3}";
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        CHECK(s.values().format == RecFormat::Opus);
        CHECK_EQ(s.values().format_preset, std::string());
        CHECK_EQ(s.save(), 0);
        const std::string body = slurp(path);
        CHECK(body.find("format_preset") == std::string::npos);
        CHECK(body.find("future_key") != std::string::npos);
    }
    {
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        s.values().format_preset = "music128";
        CHECK_EQ(s.save(), 0);
        SettingsStore t(ops, path);
        CHECK_EQ(t.load(), 0);
        CHECK(t.values().format == RecFormat::Opus);
        CHECK_EQ(t.values().format_preset, std::string("music128"));
        /* back to the default: the key goes away */
        t.values().format_preset.clear();
        CHECK_EQ(t.save(), 0);
        CHECK(slurp(path).find("format_preset") == std::string::npos);
    }
    {
        /* invalid values are ignored */
        std::ofstream(path) << "{\"version\":1,\"format\":\"opus\",\"format_preset\":\"Speech 24\"}";
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        CHECK_EQ(s.values().format_preset, std::string());
        std::ofstream(path) << "{\"version\":1,\"format\":\"flac\",\"format_preset\":7}";
        CHECK_EQ(s.load(), 0);
        CHECK_EQ(s.values().format_preset, std::string());
        CHECK(s.values().format == RecFormat::Flac);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

/* ---- the three parameters: codec, sample rate, quality ---- */

FormatInputs card_in(std::vector<uint32_t> rates, unsigned ch, std::vector<SampleFormat> fmts, CodecAvailability libs) {
    return inputs_for_offer(offer(std::move(rates), ch, ch, std::move(fmts)), libs);
}

RecStored stored(RecFormat c, const char *preset = "", int rate = 0, int quality = 0) {
    RecStored s;
    s.codec = c;
    s.preset = preset;
    s.rate = rate;
    s.quality = quality;
    return s;
}

/* every legal combination per codec on a card with all the common rates, mono, 16 and 24-bit */
void test_params_all_combinations() {
    const std::vector<uint32_t> rates = {8000, 12000, 16000, 24000, 44100, 48000, 96000};
    const FormatInputs in = card_in(rates, 1, {SampleFormat::S16_LE, SampleFormat::S24_3LE}, both());
    CHECK(in.has16 && in.has24);
    CHECK(card_rates(in) == rates);
    CHECK(card_bits(in) == (std::vector<unsigned>{16, 24}));
    /* WAV and FLAC: every rate, both depths */
    for (RecFormat f : {RecFormat::Wav, RecFormat::Flac}) {
        CHECK(codec_usable(in, f));
        CHECK(codec_rates(in, f) == rates);
        for (uint32_t r : rates)
            for (int bits : {16, 24}) {
                const RecParams p = resolve_params(in, stored(f, "", static_cast<int>(r), bits));
                CHECK_EQ(p.rate, r);
                CHECK_EQ(p.bits, static_cast<unsigned>(bits));
                const FormatChoice c = choice_for(in, p);
                CHECK(c.compatible);
                const uint64_t pcm = uint64_t(r) * (bits / 8) * 60;
                if (f == RecFormat::Wav) {
                    CHECK(c.size.kind == SizeKind::Exact);
                    CHECK_EQ(c.size.bytes_per_minute, pcm);
                } else {
                    CHECK(c.size.kind == SizeKind::About);
                    CHECK(c.size.bytes_per_minute < pcm && c.size.low_per_minute <= c.size.bytes_per_minute &&
                          c.size.bytes_per_minute <= c.size.high_per_minute);
                    CHECK_EQ(c.flac_level, 5u);
                }
            }
    }
    /* Opus: 8, 12, 16, 24, 48 kHz only; every bit rate of the list */
    CHECK(codec_usable(in, RecFormat::Opus));
    CHECK(codec_rates(in, RecFormat::Opus) == (std::vector<uint32_t>{8000, 12000, 16000, 24000, 48000}));
    CHECK(!rate_legal(RecFormat::Opus, 44100) && !rate_legal(RecFormat::Opus, 96000));
    CHECK(rate_legal(RecFormat::Wav, 44100) && rate_legal(RecFormat::Flac, 96000));
    for (uint32_t r : {8000u, 12000u, 16000u, 24000u, 48000u})
        for (int k : kOpusKbps) {
            const RecParams p = resolve_params(in, stored(RecFormat::Opus, "", static_cast<int>(r), k));
            CHECK_EQ(p.rate, r);
            CHECK_EQ(p.kbps, k);
            const FormatChoice c = choice_for(in, p);
            CHECK(c.compatible);
            CHECK(c.size.kind == SizeKind::Target);
            CHECK_EQ(c.opus_bitrate_bps, k * 1000);
            CHECK_EQ(c.size.bytes_per_minute, opus_bytes_per_minute(k * 1000));
            CHECK_EQ(c.opus_force_mono, k <= 32);
            CHECK(c.opus_app == (k <= 32 ? OpusApplication::Speech : OpusApplication::Music));
        }
    /* a rate the codec cannot use, or a value not in the list, falls to the default */
    CHECK_EQ(resolve_params(in, stored(RecFormat::Opus, "", 44100)).rate, 48000u);
    CHECK_EQ(resolve_params(in, stored(RecFormat::Opus, "", 0, 40)).kbps, 32);
    CHECK_EQ(resolve_params(in, stored(RecFormat::Wav, "", 12345)).rate, 48000u);
    CHECK_EQ(resolve_params(in, stored(RecFormat::Wav, "", 0, 20)).bits, 16u);
    CHECK_EQ(opus_kbps_ok(16) && opus_kbps_ok(128) && !opus_kbps_ok(0) && !opus_kbps_ok(40), true);
}

void test_params_defaults_and_old_files() {
    const FormatInputs in = card_in({16000, 44100, 48000}, 1, {SampleFormat::S16_LE}, both());
    /* an old file: format and preset only. The same capture as before: 48 kHz, 16-bit; Opus at the preset's bit rate */
    RecParams p = resolve_params(in, stored(RecFormat::Wav));
    CHECK(p.rate == 48000 && p.bits == 16);
    p = resolve_params(in, stored(RecFormat::Opus, "speech24"));
    CHECK(p.rate == 48000 && p.kbps == 24);
    p = resolve_params(in, stored(RecFormat::Opus, "music128"));
    CHECK(p.kbps == 128);
    p = resolve_params(in, stored(RecFormat::Opus));
    CHECK(p.kbps == 32); /* no preset: speech32 */
    p = resolve_params(in, stored(RecFormat::Opus, "gibberish"));
    CHECK(p.kbps == 32);
    /* the quality key beats the preset */
    p = resolve_params(in, stored(RecFormat::Opus, "speech24", 0, 96));
    CHECK(p.kbps == 96);
    /* the default rate: WAV and FLAC as open_policy, Opus 48 then 16 then the rest */
    CHECK_EQ(default_rate(in, RecFormat::Wav), 48000u);
    CHECK_EQ(default_rate(in, RecFormat::Opus), 48000u);
    const FormatInputs two = card_in({16000, 44100}, 1, {SampleFormat::S16_LE}, both());
    CHECK_EQ(default_rate(two, RecFormat::Wav), 44100u);
    CHECK_EQ(default_rate(two, RecFormat::Opus), 16000u);
    const FormatInputs odd = card_in({8000, 12000, 24000, 96000}, 1, {SampleFormat::S16_LE}, both());
    CHECK_EQ(default_rate(odd, RecFormat::Opus), 24000u);
    CHECK_EQ(default_rate(odd, RecFormat::Wav), 24000u); /* the highest at or below 48 kHz */
    /* the same capture rate and bits the recorder opens with (choose_params) */
    for (const CaptureOffer &o : {offer({16000, 44100, 48000}, 1, 1, {SampleFormat::S16_LE}),
                                  offer({44100}, 2, 2, {SampleFormat::S16_LE}),
                                  offer({48000, 96000}, 2, 2, {SampleFormat::S24_3LE}),
                                  offer({16000}, 1, 1, {SampleFormat::S16_LE})}) {
        const FormatInputs fi = inputs_for_offer(o, both());
        const RecParams rp = resolve_params(fi, stored(RecFormat::Wav));
        CHECK_EQ(rp.rate, fi.capture.rate);
        CHECK_EQ(rp.bits, fi.capture.bits);
    }
    /* the default depth: 16 when the card has it, else 24 */
    CHECK_EQ(default_bits(card_in({48000}, 1, {SampleFormat::S24_3LE}, both())), 24u);
    CHECK_EQ(default_bits(card_in({48000}, 1, {SampleFormat::S32_LE}, both())), 24u);
    CHECK_EQ(default_bits(card_in({48000}, 1, {SampleFormat::S16_LE, SampleFormat::S24_3LE}, both())), 16u);
    /* a 24-bit choice on a card with S32 only (converted to packed 24) */
    const FormatInputs s32 = card_in({48000}, 2, {SampleFormat::S16_LE, SampleFormat::S32_LE}, both());
    CHECK(s32.has24);
    CHECK_EQ(resolve_params(s32, stored(RecFormat::Wav, "", 0, 24)).bits, 24u);
    /* 24-bit asked of a card without it: 16 */
    CHECK_EQ(resolve_params(in, stored(RecFormat::Flac, "", 0, 24)).bits, 16u);
}

/* The 44.1 kHz-only card: Opus is dimmed (never resampled), with a reason that names the card's rate */
void test_params_44k_only() {
    const FormatInputs in = card_in({44100}, 2, {SampleFormat::S16_LE}, both());
    std::string why;
    CHECK(codec_usable(in, RecFormat::Wav) && codec_usable(in, RecFormat::Flac));
    CHECK(!codec_usable(in, RecFormat::Opus, &why));
    CHECK_EQ(why, "Opus needs 8, 12, 16, 24 or 48 kHz; this card has 44.1 kHz");
    CHECK(codec_rates(in, RecFormat::Opus).empty());
    CHECK_EQ(default_rate(in, RecFormat::Opus), 0u);
    CHECK_EQ(default_rate(in, RecFormat::Wav), 44100u);
    const FormatChoice c = choice_for(in, resolve_params(in, stored(RecFormat::Opus, "speech32")));
    CHECK(!c.compatible);
    CHECK_EQ(c.why_not, why);
    CHECK(c.size.kind == SizeKind::Target && c.size.bytes_per_minute > 0); /* the figure stays for the dimmed row */
    /* the safety net: a stored Opus choice records as WAV at 44.1 kHz */
    bool fell = false;
    const FormatChoice e = effective_for(in, stored(RecFormat::Opus, "speech32"), &fell);
    CHECK(fell && e.compatible && e.key.format == RecFormat::Wav);
    CHECK_EQ(effective_params(in, stored(RecFormat::Opus, "music64")).rate, 44100u);
    CHECK_EQ(effective_params(in, stored(RecFormat::Opus, "music64")).codec == RecFormat::Wav, true);
    /* the Opus codec change from the settings: no change offered, stored unchanged */
    const StoredChange ch = change_codec(in, stored(RecFormat::Wav), RecFormat::Opus);
    CHECK(ch.stored.codec == RecFormat::Opus && ch.stored.rate == 0); /* the caller dims the row first */
    /* two rates, none legal for Opus */
    const FormatInputs two = card_in({22050, 44100}, 1, {SampleFormat::S16_LE}, both());
    CHECK(!codec_usable(two, RecFormat::Opus, &why));
    CHECK_EQ(why, "Opus needs 8, 12, 16, 24 or 48 kHz; this card has 22.05 or 44.1 kHz");
    /* more than two channels: no Opus even at 48 kHz */
    FormatInputs six = card_in({48000}, 1, {SampleFormat::S16_LE}, both());
    six.capture.channels = 6; /* the open policy never gets here; the model still refuses */
    CHECK(!codec_usable(six, RecFormat::Opus, &why) && why == "Opus records at most 2 channels");
}

void test_params_revalidation() {
    const FormatInputs in = card_in({16000, 44100, 48000, 96000}, 1, {SampleFormat::S16_LE, SampleFormat::S24_3LE}, both());
    /* WAV 44.1 -> Opus: the nearest legal rate, 48 kHz, and a line that says so */
    StoredChange c = change_codec(in, stored(RecFormat::Wav, "", 44100), RecFormat::Opus);
    CHECK(c.stored.codec == RecFormat::Opus && c.stored.rate == 48000);
    CHECK_EQ(c.message, "Sample rate set to 48 kHz: Opus cannot record at 44.1 kHz");
    /* 96 kHz -> 48 kHz too */
    c = change_codec(in, stored(RecFormat::Flac, "", 96000), RecFormat::Opus);
    CHECK(c.stored.rate == 48000 && !c.message.empty());
    /* a legal rate stays, no message */
    c = change_codec(in, stored(RecFormat::Wav, "", 16000), RecFormat::Opus);
    CHECK(c.stored.rate == 16000 && c.message.empty());
    /* the default rate (0) of a card whose WAV default is 44.1 kHz: Opus lands on 16 kHz (no 48) */
    const FormatInputs two = card_in({16000, 44100}, 1, {SampleFormat::S16_LE}, both());
    c = change_codec(two, stored(RecFormat::Wav), RecFormat::Opus);
    CHECK(c.stored.rate == 16000);
    CHECK_EQ(c.message, "Sample rate set to 16 kHz: Opus cannot record at 44.1 kHz");
    /* back to WAV: the rate stays where Opus left it, nothing to say */
    c = change_codec(two, c.stored, RecFormat::Wav);
    CHECK(c.stored.codec == RecFormat::Wav && c.stored.rate == 16000 && c.message.empty());
    /* quality: bit depth survives WAV <-> FLAC, kbps and bits never mix */
    c = change_codec(in, stored(RecFormat::Wav, "", 0, 24), RecFormat::Flac);
    CHECK(c.stored.quality == 24);
    c = change_codec(in, stored(RecFormat::Wav, "", 0, 24), RecFormat::Opus);
    CHECK_EQ(c.stored.quality, 0);
    CHECK_EQ(resolve_params(in, c.stored).kbps, 32);
    c = change_codec(in, stored(RecFormat::Opus, "", 0, 64), RecFormat::Wav);
    CHECK_EQ(c.stored.quality, 0);
    CHECK_EQ(resolve_params(in, c.stored).bits, 16u);
    /* the same codec again: no change */
    c = change_codec(in, stored(RecFormat::Opus, "music64", 16000, 96), RecFormat::Opus);
    CHECK(c.stored.preset == "music64" && c.stored.rate == 16000 && c.stored.quality == 96 && c.message.empty());
    /* a preset: codec and bit rate, everything else back to the defaults */
    c = change_preset(stored(RecFormat::Wav, "", 44100, 24), FormatKey{RecFormat::Opus, "speech24"});
    CHECK(c.stored.codec == RecFormat::Opus && c.stored.preset == "speech24" && c.stored.rate == 0 && c.stored.quality == 0);
    CHECK_EQ(resolve_params(in, c.stored).kbps, 24);
    CHECK_EQ(resolve_params(in, c.stored).rate, 48000u);
    /* a plain bit rate is no longer a preset's */
    c = change_quality(stored(RecFormat::Opus, "speech24"), 96);
    CHECK(c.stored.preset.empty() && c.stored.quality == 96);
    c = change_rate(stored(RecFormat::Opus, "speech24", 0, 0), 16000);
    CHECK(c.stored.rate == 16000 && c.stored.preset == "speech24");
    /* presets keep their meaning: key, bit rate, mode */
    const FormatChoice p = choice_for(in, resolve_params(in, stored(RecFormat::Opus, "music64")));
    CHECK(p.key == (FormatKey{RecFormat::Opus, "music64"}) && p.opus_bitrate_bps == 64000 && !p.opus_force_mono);
    const FormatChoice q = choice_for(in, resolve_params(in, stored(RecFormat::Opus, "", 0, 48)));
    CHECK(q.key == (FormatKey{RecFormat::Opus, ""}) && q.opus_bitrate_bps == 48000);
    /* the six presets as list_formats gives them equal their three-parameter form */
    const std::vector<FormatChoice> old = list_formats(in);
    for (const FormatChoice &o : old) {
        const FormatChoice n = choice_for(in, resolve_params(in, stored(o.key.format, o.key.preset.c_str())));
        CHECK_EQ(n.compatible, o.compatible);
        CHECK_EQ(n.opus_bitrate_bps, o.opus_bitrate_bps);
        CHECK(n.opus_app == o.opus_app || o.key.format != RecFormat::Opus);
        CHECK_EQ(n.opus_force_mono, o.opus_force_mono);
        CHECK_EQ(n.size.bytes_per_minute, o.size.bytes_per_minute);
    }
}

void test_params_libraries_and_nocard() {
    CodecAvailability none;
    FormatInputs in = card_in({48000}, 1, {SampleFormat::S16_LE}, none);
    std::string why;
    CHECK(codec_usable(in, RecFormat::Wav));
    CHECK(!codec_usable(in, RecFormat::Flac, &why) && why == "FLAC not installed (libflac14)");
    CHECK(!codec_usable(in, RecFormat::Opus, &why) && why == "Opus not installed (libopusenc0)");
    bool fell = false;
    CHECK(effective_for(in, stored(RecFormat::Flac), &fell).key.format == RecFormat::Wav && fell);
    /* no card: nothing known, no rates, only the libraries decide */
    FormatInputs nc;
    nc.codecs = both();
    CHECK(card_rates(nc).empty());
    CHECK(codec_usable(nc, RecFormat::Opus));
    CHECK_EQ(default_rate(nc, RecFormat::Wav), 0u);
    CHECK(choice_for(nc, resolve_params(nc, stored(RecFormat::Flac))).size.kind == SizeKind::Unknown);
    /* a card that cannot record: nothing */
    FormatInputs un = inputs_for_offer(offer({48000}, 8, 8, {SampleFormat::S16_LE}), both());
    CHECK(!codec_usable(un, RecFormat::Wav, &why) && !why.empty());
    CHECK(!effective_for(un, stored(RecFormat::Wav)).compatible);
    /* a busy card (the sound server's 48 kHz, no offer): the capture rate is the only one */
    FormatInputs busy;
    busy.state = CaptureState::Known;
    busy.capture.rate = 48000;
    busy.capture.channels = 1;
    busy.capture.bits = 16;
    busy.codecs = both();
    CHECK(card_rates(busy) == (std::vector<uint32_t>{48000}));
    CHECK(codec_usable(busy, RecFormat::Opus));
    /* the observed FLAC ratio applies at the depth it was observed at only */
    FormatInputs ob = card_in({48000}, 1, {SampleFormat::S16_LE, SampleFormat::S24_3LE}, both());
    ob.flac_ratio = 0.62;
    CHECK_EQ(choice_for(ob, resolve_params(ob, stored(RecFormat::Flac))).size.bytes_per_minute, uint64_t(5760000 * 0.62 + 0.5));
    const FormatChoice f24 = choice_for(ob, resolve_params(ob, stored(RecFormat::Flac, "", 0, 24)));
    CHECK(f24.size.bytes_per_minute != uint64_t(8640000 * 0.62 + 0.5));
}

void test_rate_labels() {
    CHECK_EQ(rate_label(48000), std::string("48 kHz"));
    CHECK_EQ(rate_label(44100), std::string("44.1 kHz"));
    CHECK_EQ(rate_label(22050), std::string("22.05 kHz"));
    CHECK_EQ(rates_label({44100}), std::string("44.1 kHz"));
    CHECK_EQ(rates_label({16000, 44100, 48000}), std::string("16, 44.1 or 48 kHz"));
}

void test_settings_rate_quality_keys() {
    const std::string dir = temp_dir();
    const std::string path = dir + "/settings.json";
    FileOps &ops = posix_file_ops();
    {
        /* an old file loads with no rate and no quality and writes neither */
        std::ofstream(path) << "{\"version\":1,\"format\":\"opus\",\"format_preset\":\"music64\"}";
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        CHECK(s.values().rate == 0 && s.values().quality == 0 && s.values().format_preset == "music64");
        CHECK_EQ(s.save(), 0);
        const std::string body = slurp(path);
        CHECK(body.find("\"rate\"") == std::string::npos && body.find("\"quality\"") == std::string::npos);
        CHECK(body.find("music64") != std::string::npos);
    }
    {
        /* the new keys round trip */
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        s.values().rate = 16000;
        s.values().quality = 96;
        CHECK_EQ(s.save(), 0);
        SettingsStore t(ops, path);
        CHECK_EQ(t.load(), 0);
        CHECK(t.values().rate == 16000 && t.values().quality == 96 && t.values().format == RecFormat::Opus);
        const RecStored st = stored_of(t.values());
        CHECK(st.codec == RecFormat::Opus && st.rate == 16000 && st.quality == 96 && st.preset == "music64");
        t.values().rate = 0;
        t.values().quality = 0;
        CHECK_EQ(t.save(), 0);
        const std::string body = slurp(path);
        CHECK(body.find("\"rate\"") == std::string::npos && body.find("\"quality\"") == std::string::npos);
    }
    {
        /* values out of range or of the wrong type: the default */
        std::ofstream(path) << "{\"version\":1,\"format\":\"flac\",\"rate\":\"fast\",\"quality\":-3}";
        SettingsStore s(ops, path);
        CHECK_EQ(s.load(), 0);
        CHECK(s.values().rate == 0 && s.values().quality == 0 && s.values().format == RecFormat::Flac);
        std::ofstream(path) << "{\"version\":1,\"rate\":5,\"quality\":99999}";
        CHECK_EQ(s.load(), 0);
        CHECK(s.values().rate == 0 && s.values().quality == 0);
        std::ofstream(path) << "{\"version\":1,\"format\":\"wav\",\"rate\":44100,\"quality\":24}";
        CHECK_EQ(s.load(), 0);
        CHECK(s.values().rate == 44100 && s.values().quality == 24);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

void test_formats() {
    test_order_and_keys();
    test_mono48_headset();
    test_stereo44();
    test_16k_speech_headset();
    test_24bit_card();
    test_8channel_card();
    test_libraries_missing();
    test_observed_ratio();
    test_settings_key();
    test_params_all_combinations();
    test_params_defaults_and_old_files();
    test_params_44k_only();
    test_params_revalidation();
    test_params_libraries_and_nocard();
    test_rate_labels();
    test_settings_rate_quality_keys();
}
