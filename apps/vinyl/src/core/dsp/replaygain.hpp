/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl ReplayGain (report 060 3d, owner decision D25 of 2026-11-13: "from tags only", Off / Track / Album, default
 * Track): the tag values a file carries, turned into the gain Vinyl's engine applies to its samples. No analysis: the
 * values come from PC tools (rsgain, foobar2000, mp3gain...). Sources (core/tags reads them):
 *
 *   ID3v2      TXXX REPLAYGAIN_TRACK_GAIN / _TRACK_PEAK / _ALBUM_GAIN / _ALBUM_PEAK (any case)
 *   Vorbis     the same names as comments (FLAC, Ogg Vorbis)
 *   Opus       R128_TRACK_GAIN / R128_ALBUM_GAIN: Q7.8 dB relative to -23 LUFS, so the ReplayGain value is
 *              v / 256 + 5 dB (ReplayGain 2's -18 LUFS reference); the header's output gain is applied by the decoder
 *              (RFC 7845), the R128 tags on top of it
 *   MP4        the freeform items ----:com.apple.iTunes:replaygain_* (iTunNORM is not used)
 *
 * The gain: Off 0 dB; Track the track gain (the album gain when the file has no track gain); Album the album gain (the
 * track gain when it has none); a file without either plays at 0 dB. A pre-amp of 0 dB for now (a pre-amp setting is
 * later, report 060 7.2 #5). Clipping prevention: the gain is never more than -20 log10(peak) when the matching peak
 * tag exists (the album peak with the album gain), so the loudest sample stays at or under full scale. The gain is
 * digital, applied by the engine to Vinyl's stream before the output's own volume: the output's levels never move.
 * LVGL-free.
 */
#pragma once

#include <string>

namespace vinyl {

enum class ReplayGainMode { Off, Track, Album };
/* "off", "track", "album" (the settings file) */
const char *replaygain_mode_name(ReplayGainMode m);
ReplayGainMode replaygain_mode_from(const std::string &s, ReplayGainMode fallback);
/* "Off", "Track", "Album" (the pages) */
const char *replaygain_mode_word(ReplayGainMode m);

struct ReplayGainTags {
    bool has_track = false, has_album = false, has_track_peak = false, has_album_peak = false;
    float track_db = 0, album_db = 0;     /* gains in dB */
    float track_peak = 0, album_peak = 0; /* linear sample peaks (1.0 = full scale) */
    bool r128_track = false, r128_album = false; /* the gain came from an Opus R128 tag (it wins over REPLAYGAIN_*) */
    bool any() const { return has_track || has_album; }
};

/* "-6.54 dB", "+1.20 dB", "-6.54" (spaces, a comma for the decimal point accepted): false when no number leads. */
bool parse_gain_db(const std::string &v, float &out);
/* "0.988553", "1.02": false when no number leads or it is negative. */
bool parse_peak(const std::string &v, float &out);
/* An Opus R128 gain tag ("-1234", Q7.8 dB relative to -23 LUFS) as a ReplayGain value: v / 256 + 5 dB. */
bool parse_r128_db(const std::string &v, float &out);

/* Applies one tag by its name (REPLAYGAIN_TRACK_GAIN ..., R128_TRACK_GAIN, R128_ALBUM_GAIN; any case); false when the
 * name is not a ReplayGain one. An R128 value wins over a REPLAYGAIN one (Opus files should carry only R128). */
bool apply_replaygain_tag(ReplayGainTags &rg, const std::string &name, const std::string &value);

constexpr double kReplayGainPreampDb = 0.0;

/* The gain in dB for the mode (0 when off or untagged), the peak guard applied. */
double replaygain_db(ReplayGainMode mode, const ReplayGainTags &rg, double preamp_db = kReplayGainPreampDb);
/* The same as an amplitude factor (1 = unchanged). */
float replaygain_factor(ReplayGainMode mode, const ReplayGainTags &rg, double preamp_db = kReplayGainPreampDb);
/* "RG -3.2 dB" ("RG 0.0 dB" when tagged at 0; "" when nothing applies: off or untagged). */
std::string replaygain_chip_text(ReplayGainMode mode, const ReplayGainTags &rg);

}  // namespace vinyl
