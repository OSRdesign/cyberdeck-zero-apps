/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: the format model for every kind of card (mono 48 kHz S16 headset, stereo 44.1 kHz, a 16 kHz-only
 * speech headset, a 24-bit card, an 8-channel card), libraries present or missing, the size model, the stored keys, and
 * the settings file's new "format_preset" key.
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
}
