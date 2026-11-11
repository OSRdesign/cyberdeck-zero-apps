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
 * The three parameters (Settings: Codec, Sample rate, Quality): "format" is the codec, "rate" the capture rate in Hz
 * (0 = the card's default for the codec), "quality" the bit depth (WAV, FLAC: 16 or 24) or the bit rate in kbps (Opus:
 * 16, 24, 32, 48, 64, 96 or 128); 0 = the default (a preset's bit rate for Opus, else 16-bit, else 32 kbps). A preset
 * sets the codec and its bit rate and leaves rate and quality at their defaults. Opus is legal at 8, 12, 16, 24 and
 * 48 kHz with at most 2 channels; a card that offers none of those has Opus dimmed (never resampled).
 * resolve_params() turns what is stored into what the card can do; effective_for() falls back to WAV when the stored
 * combination cannot record (a card was swapped, a library is gone).
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
    /* What the card offers (the Rate and Quality pickers): the rates and whether it has 16-bit and 24-bit capture. Empty
     * rates: only capture.rate is known (a busy card recorded through the sound server). */
    std::vector<uint32_t> rates;
    bool has16 = true, has24 = false;
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

/* ---- the three parameters: codec, sample rate, quality ---- */

/* What settings.json holds ("format", "format_preset", "rate", "quality"). */
struct RecStored {
    RecFormat codec = RecFormat::Wav;
    std::string preset;
    int rate = 0;
    int quality = 0;
};
RecStored stored_of(const Settings &s);

/* A resolved choice. rate 0: the codec has no legal rate on this card. */
struct RecParams {
    RecFormat codec = RecFormat::Wav;
    uint32_t rate = 0;
    unsigned bits = 16; /* the capture depth (Opus: what the card delivers) */
    int kbps = 0;       /* Opus */
    bool operator==(const RecParams &o) const {
        return codec == o.codec && rate == o.rate && bits == o.bits && kbps == o.kbps;
    }
};

/* The bit rates the Quality row offers for Opus (kbps), in order. */
extern const int kOpusKbps[7];
bool opus_kbps_ok(int kbps);
/* 24, 32, 64, 128 for the presets speech24, speech32, music64, music128; 0 for anything else. */
int preset_kbps(const std::string &preset);
/* The preset whose bit rate is kbps ("" when none). */
std::string preset_of_kbps(int kbps);
/* Up to 32 kbps Opus codes speech (VOIP mode, one channel on a stereo card), above it music (AUDIO mode). */
bool kbps_is_speech(int kbps);

/* "48 kHz", "44.1 kHz", "22.05 kHz" */
std::string rate_label(uint32_t rate);
/* "44.1 kHz", "44.1 or 48 kHz", "16, 44.1 or 48 kHz" */
std::string rates_label(const std::vector<uint32_t> &rates);

/* What the card offers, ascending (empty: no card or a card that cannot record). */
std::vector<uint32_t> card_rates(const FormatInputs &in);
/* The bit depths the card can record: 16 and / or 24 (16 when nothing is known). */
std::vector<unsigned> card_bits(const FormatInputs &in);
/* Is the codec legal at this rate (Opus: 8, 12, 16, 24, 48 kHz)? false with the reason. */
bool rate_legal(RecFormat codec, uint32_t rate, std::string *why = nullptr);
/* The card's rates the codec can use, ascending. */
std::vector<uint32_t> codec_rates(const FormatInputs &in, RecFormat codec);
/* Can the codec record on this card at all (the Codec picker dims it when not)? false with the reason. */
bool codec_usable(const FormatInputs &in, RecFormat codec, std::string *why = nullptr);
/* The rate used when none is chosen: WAV and FLAC as open_policy's choose_rate (48, 44.1, the highest below, else the
 * lowest); Opus 48, then 16, then the highest of 24, 12, 8 the card has. 0: none. */
uint32_t default_rate(const FormatInputs &in, RecFormat codec);
/* The rate to land on when the codec changes: `from` when the codec can use it, else for Opus 48 then 16 kHz, else the
 * nearest. 0: none. */
uint32_t nearest_rate(const FormatInputs &in, RecFormat codec, uint32_t from);
unsigned default_bits(const FormatInputs &in); /* 16 when the card has it, else 24 */

RecParams resolve_params(const FormatInputs &in, const RecStored &st);
/* The full entry for a resolved choice: encoder parameters, compatible (+ why_not), the size per minute at that rate,
 * depth and bit rate. name: "WAV", "FLAC", "Opus". */
FormatChoice choice_for(const FormatInputs &in, const RecParams &p);
/* What to record with: the stored combination when it can record, else WAV (in WAV's default rate and depth); *fell_back
 * says which. compatible is false (no WAV either) when nothing records. */
FormatChoice effective_for(const FormatInputs &in, const RecStored &st, bool *fell_back = nullptr);
/* The same as parameters (what the capture is opened with). */
RecParams effective_params(const FormatInputs &in, const RecStored &st, bool *fell_back = nullptr);

/* A change of one parameter: the new stored values and a one-line notice naming what else changed ("" when nothing). */
struct StoredChange {
    RecStored stored;
    std::string message;
};
StoredChange change_codec(const FormatInputs &in, const RecStored &cur, RecFormat codec);
/* A quick preset: the codec and its bit rate; rate and quality back to their defaults. */
StoredChange change_preset(const RecStored &cur, const FormatKey &preset);
StoredChange change_rate(const RecStored &cur, uint32_t rate);
StoredChange change_quality(const RecStored &cur, int quality);

/* The size model, exposed for the tests and the docs. */
constexpr double kFlacRatioSpeech16 = 0.30; /* P0-e: 27 % on real mono speech at level 5; tests/codec measurements */
constexpr double kFlacRatioMusic16 = 0.55;  /* synthetic music at level 5 (docs/FORMATS.md: measured spread) */
double flac_ratio_for_bits(double ratio16, unsigned bits);
uint64_t pcm_bytes_per_minute(const AudioFormat &fmt);
uint64_t opus_bytes_per_minute(int bitrate_bps);
constexpr uint64_t kOpusHeaderBytes = 700;  /* OpusHead page + OpusTags page with 512 bytes of padding, about */
constexpr uint64_t kFlacHeaderBytes = 4400; /* fLaC + STREAMINFO + comments + 4096 bytes of padding, about */

}  // namespace cassette
