/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the recording formats the Settings Format row can offer (pure model, no UI, no library calls).
 * Input: the capture format of the selected card (what open_policy chooses from the card's offer) and which encoder
 * libraries load. Output: every format in a stable order, each with its stored key, a name and a one-line detail
 * (wording is a placeholder for the owner), whether it works with this card (and why not), and the estimated size per
 * minute at the current capture format.
 *
 * Offered (docs/FORMATS.md explains the choice):
 *   wav             WAV/BWF, uncompressed (the default; always works when the card records at all)
 *   flac            FLAC level 5, lossless
 *   opus-speech24   Opus 24 kbps, speech (VOIP mode, one coded channel)
 *   opus-speech32   Opus 32 kbps, speech (VOIP mode, one coded channel)
 *   opus-music64    Opus 64 kbps, music (AUDIO mode)
 *   opus-music128   Opus 128 kbps, music (AUDIO mode)
 * Stored as settings.json "format": "wav"|"flac"|"opus" (unchanged) plus the optional "format_preset":
 * ""|"speech24"|"speech32"|"music64"|"music128". A missing or unknown preset means the format's default (Opus:
 * speech32), so files written before this key existed keep their meaning.
 *
 * Size model (bytes per minute, after the file's one-off header):
 *   WAV   exact: rate x channels x bytes x 60 (bytes_per_hour of takes/recordings, the time-left logic); header 5120
 *   Opus  the bitrate target plus Ogg page overhead (one page per second and the lacing bytes); VBR, so a take with
 *         pauses comes out smaller than the target, never much larger
 *   FLAC  "about": PCM x the measured ratio (16-bit speech 0.30, 16-bit music 0.55; 24-bit adds the 8 noise bits a
 *         card's low bits always carry: (r16 x 16 + 8) / 24); a running take's observed ratio replaces the default
 */
#pragma once

#include "../alsa/open_policy.hpp"
#include "../takes/settings.hpp"
#include "codec_libs.hpp"
#include "opus_encoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

/* What settings.json stores for a format choice. */
struct FormatKey {
    RecFormat format = RecFormat::Wav;
    std::string preset; /* "" = the format's default */
    bool operator==(const FormatKey &o) const { return format == o.format && preset == o.preset; }
};

enum class SizeKind {
    Unknown, /* no capture format (no card): no figure */
    Exact,   /* WAV */
    Target,  /* Opus: the bitrate target */
    About,   /* FLAC: an estimate from a compression ratio */
};

struct SizeEstimate {
    SizeKind kind = SizeKind::Unknown;
    uint64_t bytes_per_minute = 0; /* the typical figure (FLAC: at the speech ratio, or the observed one) */
    uint64_t low_per_minute = 0;   /* FLAC: the range speech .. music; others: = bytes_per_minute */
    uint64_t high_per_minute = 0;
    uint64_t header_bytes = 0;     /* once per file */
    /* header + duration x bytes_per_minute */
    uint64_t bytes_for_seconds(double seconds) const;
};

struct FormatChoice {
    std::string id; /* stable: "wav", "flac", "opus-speech24", ... */
    FormatKey key;
    std::string name;   /* "WAV", "FLAC", "Opus 24 kbps" */
    std::string detail; /* one line: "uncompressed, plays everywhere" */
    bool compatible = false;
    std::string why_not; /* short, when not compatible: "Opus cannot record at 44.1 kHz" */
    SizeEstimate size;
    /* encoder parameters */
    unsigned flac_level = 0;
    int opus_bitrate_bps = 0;
    OpusApplication opus_app = OpusApplication::Speech;
    bool opus_force_mono = false; /* speech presets: one coded channel on a stereo card */
};

enum class CaptureState {
    NoCard,   /* no card selected or present: compatibility by library only, no sizes */
    Unusable, /* a card the open policy cannot record from: nothing is compatible (capture_why says why) */
    Known,    /* capture holds the format the recorder will capture in */
};

struct FormatInputs {
    CaptureState state = CaptureState::NoCard;
    AudioFormat capture;
    std::string capture_why;
    CodecAvailability codecs;
    double flac_ratio = 0; /* an observed FLAC ratio (bytes / PCM bytes) of the current card, 0: the model's */
};

/* Every format, in the stable order above. */
std::vector<FormatChoice> list_formats(const FormatInputs &in);

/* The inputs for a card's capture offer: open_policy's choose_params() decides the capture format (rate, channels, bits)
 * exactly as the recorder will open the card. */
FormatInputs inputs_for_offer(const CaptureOffer &offer, const CodecAvailability &codecs);

/* The entry of a stored key (an unknown or empty preset: the format's default entry); null if none. */
const FormatChoice *find_choice(const std::vector<FormatChoice> &list, const FormatKey &key);
/* What to record with: the stored choice when compatible, else WAV (when WAV is not compatible either, nothing records:
 * null). */
const FormatChoice *effective_choice(const std::vector<FormatChoice> &list, const FormatKey &stored);

/* Hours of recording that fit in free_bytes (the one-off header ignored, as takes/recordings hours_left does);
 * 0 without a figure. For WAV this equals hours_left(free_bytes, capture). */
double hours_for_space(uint64_t free_bytes, const SizeEstimate &size);

/* The size model, exposed for the tests and the docs. */
constexpr double kFlacRatioSpeech16 = 0.30; /* P0-e: 27 % on real mono speech at level 5; tests/codec measurements */
constexpr double kFlacRatioMusic16 = 0.55;  /* synthetic music at level 5 (docs/FORMATS.md: measured spread) */
double flac_ratio_for_bits(double ratio16, unsigned bits);
uint64_t pcm_bytes_per_minute(const AudioFormat &fmt);
uint64_t opus_bytes_per_minute(int bitrate_bps);
constexpr uint64_t kOpusHeaderBytes = 700;  /* OpusHead page + OpusTags page with 512 bytes of padding, about */
constexpr uint64_t kFlacHeaderBytes = 4400; /* fLaC + STREAMINFO + comments + 4096 bytes of padding, about */

}  // namespace cassette
