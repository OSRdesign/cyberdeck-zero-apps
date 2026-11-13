/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl volume: the level of the output Vinyl plays on (decision D11 as the owner changed it on 2026-11-12), in steps
 * fitted to that output (the owner, 2026-11-13: "50% should be the real mid output of the soundcard/DAC ... let's read
 * the output characteristics and use steps adapted for each output ... each step should be 1-2db Increment across the
 * full range of the DAC/output", after https://www.dr-lex.be/info-stuff/implement-a-volume-control.html).
 *
 * OutputVolumeModel, one per output:
 *   the window   the output's real dB range, at most its upper 60 dB (Dr. Lex: consumer equipment has a usable range
 *                of about 60 dB): [max(lo, hi - 60), hi], hi the output's real top (the Raspberry Pi jack's +4.00 dB
 *                included: 100 % is the output's own top, never above it);
 *   the levels   0 = mute (the switch off, the sink muted, or the control's mute value: "Muted"); 1 = the window's bottom
 *                (the first level that sounds), N = its top; linear in dB between them: dB(i) = lo + (i - 1) (hi - lo) /
 *                (N - 1), N = ceil(window / 2 dB) (the coordinator's rule), at least 2, the step never under 1 dB nor
 *                under the control's own step; the percent shown is round(100 i / N), so a press moves one level, not 5 %.
 *
 * Where the window comes from:
 *   an ALSA card (no server)     its playback mixer control ("Master", "PCM", "Speaker", "Headphone", "Headset", else any
 *                                "... Playback Volume"): the dB of its lowest sounding value and of its max (the TLV;
 *                                a dB scale with its mute flag is fixed in core/alsa); set by raw value, read back fresh;
 *                                the control's "... Playback Switch" mutes. A control without dB data: its raw steps;
 *   a sink of the sound server   its own volume in the server's dB (pa_sw_volume_from_dB: v = NORM * 10^(dB / 60)): the
 *                                window is the backing card's mixer range moved under the sink's 0 dB (the server puts
 *                                the hardware's top at the sink's 100 %), so a DAC whose floor is -45 dB never gets a
 *                                target below it; a sink without a known card (Bluetooth, software sinks): -60..0 dB;
 *   a card without a mixer       Vinyl's software gain over -60..0 dB, kept per output (output_volume_key) in state.json
 *                                as its percent.
 * The same output (the same card model, the same dB range) gets the same table on every board, through a server or not.
 *
 * The pure helpers below are tested with a fake mixer and fake sinks; the controller (vinyl_app.cpp) reads the sinks from
 * the server's lists (refreshed on its change events, app/sink_watch.hpp) and the mixer every 300 ms. LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vinyl {

constexpr double kVolumeWindowDb = 60; /* the usable range kept at the top of an output's range */
constexpr double kVolumeStepDb = 2;    /* the step aimed at: N = ceil(window / 2 dB) */
constexpr double kVolumeMinStepDb = 1; /* a step is never smaller */

/* The levels of one output (see above). */
struct OutputVolumeModel {
    double lo_db = -60, hi_db = 0; /* the window */
    int n = 30;                    /* levels 1..n above mute (0) */
    bool db = true;                /* false: a control without dB data (its levels are raw steps, lo/hi meaningless) */
    double step_db() const { return n > 1 ? (hi_db - lo_db) / (n - 1) : 0.0; }
    double db_of(int level) const;    /* level 1..n (clamped) */
    int pct_of(int level) const;      /* round(100 level / n); 0 for level <= 0 */
    int nearest(double db) const;     /* the level nearest a dB value: 1..n */
    int from_pct(int pct) const;      /* the level a stored percent stands for: 0..n */
    int step(int level, int delta) const { return level + delta < 1 ? 1 : (level + delta > n ? n : level + delta); }
};
/* The model of a range: lo the dB of the lowest sounding value, hi the top; native_step_db the control's own step (0:
 * continuous). A range wider than 60 dB keeps its upper 60 dB. */
OutputVolumeModel volume_model(double lo_db, double hi_db, double native_step_db = 0);
/* The window of an output whose range is not known: -60..0 dB. */
OutputVolumeModel default_volume_model();

/* The playback volume control Vinyl uses on a card. */
struct MixerChoice {
    std::string volume;          /* "Headset Playback Volume" */
    std::string sw;              /* "Headset Playback Switch", "" when the card has none */
    long min = 0, max = 0;       /* raw range */
    bool has_db = false;         /* the driver reports dB */
    long db_min = 0, db_max = 0; /* hundredths of a dB of min and max, when has_db (core/alsa fixed a mute flag's min) */
    bool min_is_mute = false;    /* the control's lowest value is mute: the lowest sound is min + 1 */
    OutputVolumeModel model;     /* its levels */
    bool ok() const { return !volume.empty() && max > min; }
};

/* The playback control of a card's control list (none: the card has no playback mixer), with its model. */
bool choose_playback_mixer(const std::vector<CtlInfo> &ctls, MixerChoice &out);
/* The dB of a raw value in hundredths (has_db only; linear in raw, the dB scale of the control). */
long mixer_cdb_for(const MixerChoice &m, long raw);
/* The raw value of a level: 1..n on the window (never the mute value, never past max); 0: the control's minimum. */
long mixer_raw_for_level(const MixerChoice &m, int level);
/* The level a raw value stands for: 0 for the mute value (min_is_mute), else the nearest level 1..n (a value under the
 * window reads 1, set elsewhere: the window keeps the top 60 dB). */
int mixer_level_for_raw(const MixerChoice &m, long raw);

/* "plughw:CARD=UC02,DEV=0" -> "hw:CARD=UC02"; "default" (and anything else) -> "default" (the default card's mixer). */
std::string mixer_card_for(const std::string &alsa_device);

struct MixerLevel {
    bool has_mixer = false; /* the card has a playback mixer control */
    int level = -1;         /* 0..n */
    int pct = -1;           /* the percent shown */
    bool muted = false;
    long cdb = 0;           /* its dB in hundredths (has_db) */
    long raw = 0;           /* the control's value */
};

/* Finds the card's playback control (cached in `choice` for `card`; a different card is looked up again) and reads it.
 * 0, or a negative error (no ALSA, no such card): `out.has_mixer` false then. */
int read_mixer(AlsaApi &alsa, const std::string &card, std::string &choice_card, MixerChoice &choice, MixerLevel &out);
/* Sets the control to a level (all channels) and its switch (on above 0, off at 0). 0 or a negative error. */
int write_mixer_level(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int level);
/* Sets only the control's switch (mute / unmute), the level kept. -ENOENT without a switch. */
int write_mixer_switch(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, bool on);

/* The sound server's dB scale (pa_sw_volume_from_dB / pa_sw_volume_to_dB: the cube of the linear factor), 0x10000 =
 * 0 dB; a volume of 0 is -infinity (silent). */
uint32_t sink_volume_for_db(double db);
double sink_db_for_volume(uint32_t volume);
/* A sink's model: its card's mixer range moved under the sink's 0 dB (card_mixer may be null or without dB: -60..0). */
OutputVolumeModel sink_volume_model(const MixerChoice *card_mixer);
/* The level of a sink volume: 0 for a volume of 0 (silent), else the nearest level. */
int sink_level_for_volume(const OutputVolumeModel &m, uint32_t volume);

/* What one press of "-" (delta < 0) or "+" does, one level a press (the owner's report from the deck, 2026-11-13: "zero7
 * is showing muted but volume bar shows 18"): while muted "-" does nothing and "+" brings the sound back at the level
 * it had (level 1 when it had none); not muted, a level, and "-" at level 1 mutes (the switch, the sink's mute, or the
 * control's mute value; a card with neither stays at level 1). */
struct VolumePress {
    enum class Kind { None, Set, Mute, Unmute } kind = Kind::None;
    int level = 0; /* Set: the level */
};
VolumePress mixer_press(const MixerChoice &m, const MixerLevel &lv, int delta);
VolumePress sink_press(const OutputVolumeModel &m, int level, bool muted, int delta);

/* The chip's text for a percent: "Vol 45", "Muted", and "Vol --" when the level is not known (negative: the sink is
 * gone, not read yet, or the server reports none): a negative value is never shown or logged (board test B18). */
std::string volume_chip_text(int pct, bool muted);
/* The level as the log writes it: "45 %", or "-- (not known)". */
std::string volume_log_text(int pct);
/* The log line of a set: "set hw:CARD=Headphones 'PCM Playback Volume' step 16/30 52 % target -24.97 dB raw=-2497 read
 * back raw=-2497"; read_result < 0: "read back failed (-19)". read_raw comes from a fresh read (the control opened again,
 * the sink asked again). */
std::string volume_set_log_text(const std::string &out, const OutputVolumeModel &m, int level, long raw, int read_result,
                                long read_raw);
/* A model in one line for the log: "-45.00..0.00 dB, 23 levels of 2.05 dB" (or "23 raw steps" without dB). */
std::string volume_model_text(const OutputVolumeModel &m);

}  // namespace vinyl
