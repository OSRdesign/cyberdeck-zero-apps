/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback VOLUME: the level of the output a take plays on (the owner's request of 2026-11-16: "While playing a take
 * there's nothing to adjust playback volume"; the owner's decision: the same model as Vinyl). Copied from Vinyl
 * (apps/vinyl/src/core/volume/output_volume.*, same author and licence) without its gain boost and its fine "Volume steps":
 * only the standard step of about 2 dB.
 *
 * OutputVolumeModel, one per output (Vinyl's "Volume levels fitted to each output", after
 * https://www.dr-lex.be/info-stuff/implement-a-volume-control.html):
 *   the window   the output's real dB range, at most its upper 60 dB: [max(lo, hi - 60), hi], hi the output's real top
 *                (100 % is the output's own top, never above it);
 *   the levels   0 = mute; 1 = the window's bottom (the first level that sounds), N = its top; linear in dB between them:
 *                dB(i) = lo + (i - 1) (hi - lo) / (N - 1), N = ceil(window / 2 dB), at least 2, a step never under 1 dB
 *                nor under the control's own step; the percent shown is round(100 i / N), so a press moves one level.
 *
 * Where the window comes from:
 *   an ALSA card (no sound server)  its playback mixer control ("Master", "PCM", "Speaker", "Headphone", "Headset", else
 *                                   another "... Playback Volume" that is not an input's loop-back: "Mic", "Line" ...):
 *                                   the dB of its lowest sounding value and of its max (a dB scale with its mute flag
 *                                   fixed: alsa_fix_mute_db); set by raw value; its "... Playback Switch" mutes. A control
 *                                   without dB data: its raw steps (at most 30);
 *   a sink of the sound server      its own volume in the server's dB (pa_sw_volume_from_dB: v = NORM * 10^(dB / 60)),
 *                                   the window the range of the ALSA card behind it moved under the sink's 0 dB; a sink
 *                                   without a known card (Bluetooth, software sinks): -60..0 dB;
 *   a card without a mixer          Cassette's software volume over -60..0 dB (the Player's gain), kept per output in
 *                                   settings.json ("playback_volume"), 100 % until changed (the take as recorded).
 * Muted (MuteBy): the switch; else the control's own mute value; else (and for a card without a mixer) Cassette's own mute:
 * the Player silent, the output's level kept (settings.json "playback_volume" ... "muted"). Every path shows the same:
 * "Mute", "-" does nothing, "+" brings the level back; "-" at level 1 mutes.
 *
 * Pure helpers (tested on the fake ALSA in tests/playback/test_volume.cpp); the controller reads the sinks from the
 * server's lists (re-listed on its change events) and the mixer every 300 ms while a take plays. LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

constexpr double kVolumeWindowDb = 60; /* the usable range kept at the top of an output's range */
constexpr double kVolumeStepDb = 2;    /* the step aimed at: N = ceil(window / 2 dB) */
constexpr double kVolumeMinStepDb = 1; /* a step is never smaller */

/* The levels of one output (see above). */
struct OutputVolumeModel {
    double lo_db = -60, hi_db = 0; /* the window */
    int n = 30;                    /* levels 1..n above mute (0) */
    bool db = true;                /* false: a control without dB data (its levels are raw steps, lo/hi meaningless) */
    double step_db() const { return n > 1 ? (hi_db - lo_db) / (n - 1) : 0.0; }
    double db_of(int level) const; /* level 1..n (clamped) */
    int pct_of(int level) const;   /* round(100 level / n); 0 for level <= 0 */
    int nearest(double db) const;  /* the level nearest a dB value: 1..n */
    int from_pct(int pct) const;   /* the level a percent stands for: 0..n */
    int step(int level, int delta) const { return level + delta < 1 ? 1 : (level + delta > n ? n : level + delta); }
};
/* The model of a range: lo the dB of the lowest sounding value, hi the top; native_step_db the control's own step (0:
 * continuous). A range wider than 60 dB keeps its upper 60 dB. */
OutputVolumeModel volume_model(double lo_db, double hi_db, double native_step_db = 0);
/* The window of an output whose range is not known, and of the software volume: -60..0 dB, 30 levels. */
OutputVolumeModel default_volume_model();

/* The playback volume control Cassette uses on a card. */
struct MixerChoice {
    std::string volume;          /* "Speaker Playback Volume" */
    std::string sw;              /* "Speaker Playback Switch", "" when the card has none */
    long min = 0, max = 0;       /* raw range */
    bool has_db = false;         /* the driver reports dB */
    long db_min = 0, db_max = 0; /* hundredths of a dB of min and max, when has_db (a mute flag's min fixed) */
    bool min_is_mute = false;    /* the control's lowest value is mute: the lowest sound is min + 1 */
    OutputVolumeModel model;     /* its levels */
    bool ok() const { return !volume.empty() && max > min; }
};

/* The playback control of a card's control list (false: the card has no playback mixer), with its model. An input's loop-back
 * into the output ("Mic Playback Volume", "Line ...", "Sidetone ...", "Loopback ...", "Monitor ...") is never taken: it is not
 * the output's level. */
bool choose_playback_mixer(const std::vector<CtlInfo> &ctls, MixerChoice &out);
/* The dB of a raw value in hundredths (has_db only; linear in raw, the dB scale of the control). */
long mixer_cdb_for(const MixerChoice &m, long raw);
/* The raw value of a level: 1..n on the window (never the mute value, never past max); 0: the control's minimum. */
long mixer_raw_for_level(const MixerChoice &m, int level);
/* The level a raw value stands for: 0 for the mute value (min_is_mute), else the nearest level 1..n. */
int mixer_level_for_raw(const MixerChoice &m, long raw);

/* "hw:CARD=UC02,DEV=0" / "plughw:CARD=UC02,DEV=0" -> "hw:CARD=UC02"; "default" (and anything else) -> "default". */
std::string mixer_card_for(const std::string &alsa_device);

struct MixerLevel {
    bool has_mixer = false; /* the card has a playback mixer control */
    int level = -1;         /* 0..n */
    int pct = -1;           /* the percent shown */
    bool muted = false;
    long cdb = 0;           /* its dB in hundredths (has_db) */
    long raw = 0;           /* the control's value */
};

/* Finds the card's playback control (cached in `choice` for `card`; another card is looked up again) and reads it. 0, or a
 * negative error (no ALSA, no such card): `out.has_mixer` false then. */
int read_mixer(AlsaApi &alsa, const std::string &card, std::string &choice_card, MixerChoice &choice, MixerLevel &out);
/* Sets the control to a level (all channels) and its switch (on above 0, off at 0). 0 or a negative error. */
int write_mixer_level(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int level);
/* Sets only the control's switch (mute / unmute), the level kept. -ENOENT without a switch. */
int write_mixer_switch(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, bool on);

/* The sound server's dB scale (pa_sw_volume_from_dB / pa_sw_volume_to_dB), 0x10000 = 0 dB; a volume of 0 is silent. */
uint32_t sink_volume_for_db(double db);
double sink_db_for_volume(uint32_t volume);
/* A sink's model: its card's mixer range moved under the sink's 0 dB (card_mixer null or without dB: -60..0). */
OutputVolumeModel sink_volume_model(const MixerChoice *card_mixer);
/* The level of a sink volume: 0 for a volume of 0 (silent), else the nearest level. */
int sink_level_for_volume(const OutputVolumeModel &m, uint32_t volume);
/* The sound server's placeholder output at start ("auto_null"): never a level. */
bool placeholder_sink(const std::string &name);

/* What one press of "-" (delta < 0) or "+" does, one level a press: muted, "-" does nothing and "+" brings the sound back at
 * the level it had (level 1 when it had none); not muted, a level, and "-" at level 1 mutes. */
struct VolumePress {
    enum class Kind { None, Set, Mute, Unmute } kind = Kind::None;
    int level = 0; /* Set: the level; Unmute: the level that sounds again */
};
/* How an ALSA output goes silent: its switch; else its control's own mute value (Set level 0); else (no switch, no mute
 * value, or no playback mixer at all) Cassette's own mute, the Player silent and the card's level kept. */
enum class MuteBy { Switch, MuteValue, Cassette };
MuteBy mute_by(const MixerChoice &m);
/* `lv.muted` includes Cassette's own mute (mixer_level_shown). Kind::Mute / Unmute mean the switch or Cassette's own mute;
 * from the control's mute value "+" sets level 1. */
VolumePress mixer_press(const MixerChoice &m, const MixerLevel &lv, int delta);
VolumePress sink_press(const OutputVolumeModel &m, int level, bool muted, int delta);

/* Cassette's own part of an output's volume, as settings.json keeps it per output: the software level of a card without a
 * playback mixer (a percent, 100 until changed) and Cassette's own mute (the level kept while muted). */
struct OwnVolume {
    int pct = 100;
    bool muted = false;
};
/* The software level (default_volume_model) of a stored percent; *muted also for a stored 0 (muted with no level to come
 * back at: "+" gives level 1). */
int software_level(const OwnVolume &v, bool *muted);
/* One press on the software volume, `v` changed: "-" at level 1 mutes (the level kept), "-" muted does nothing, "+" muted
 * un-mutes at the level, else one level. */
VolumePress software_press(OwnVolume &v, int delta);
/* The card's level as the stepper shows it: muted too while Cassette's own mute holds on a control without a switch or a mute
 * value. */
MixerLevel mixer_level_shown(const MixerChoice &m, const MixerLevel &lv, bool own_muted);
/* The software gain (an amplitude) of a level in dB: 10^(dB / 20). */
float volume_gain_for_db(double db);

/* ---- the output whose volume the stepper sets ---- */

/* Where a take plays and how its volume is set: a sink of the sound server (a Bluetooth device; the default output through
 * the server), or an ALSA card's mixer (a card, or ALSA's "default" without a server); None while it is not known (the
 * Bluetooth device has no sink now, the server's default sink is not known yet). `key` names the output in settings.json:
 * "bt:AA:BB:CC:DD:EE:FF" (whatever its node's name), "pulse:<sink>", "alsa:hw:CARD=<id>" or "alsa:default". */
struct VolumeTarget {
    enum class Kind { None, Sink, Mixer } kind = Kind::None;
    std::string sink;      /* Sink: the sink's name */
    unsigned channels = 2; /* Sink: its channels (the caller fills them from the sink list) */
    std::string card;      /* Mixer: "hw:CARD=<id>" or "default" */
    std::string key;       /* "" for None without a key */
    bool operator==(const VolumeTarget &o) const {
        return kind == o.kind && sink == o.sink && card == o.card && key == o.key;
    }
};
/* `device` the output in use (OutputOption::device: "hw:CARD=X,DEV=0", "default", "bt:<MAC>"); `bt_sink` the Bluetooth device's
 * sink now ("" when none); `server` a sound server answers (its sinks are listed); `server_expected` one runs on this board
 * even if it does not answer now (then "default" is not known rather than a mixer); `default_sink` the server's default sink. */
VolumeTarget volume_target_for(const std::string &device, bool bluetooth, const std::string &bt_sink, bool server,
                               bool server_expected, const std::string &default_sink);

/* ---- the first use of an output ---- */

/* The first time Cassette plays to an output (no "playback_volume" entry for its key yet) and the output is silent (muted, or at
 * level 0 or 1, the bottom of its window), its level is set near 50 % (the owner's decision of 2026-11-16, "first-use of a
 * silent output starting near 50 percent"); after that the output keeps its own level. Not for the software volume (100 %
 * until changed: never silent at first use). */
constexpr int kFirstUsePct = 50;
bool first_use_silent(int level, bool muted);

/* ---- texts ---- */

/* The word of a muted level (Vinyl's, the owner: "Mute is better than Muted"). */
constexpr const char *kMuteWord = "Mute";
/* The level as the log writes it: "45 %", or "-- (not known)". */
std::string volume_log_text(int pct);
/* The log line of a set: "set hw:CARD=X 'Speaker Playback Volume' step 16/23 70 % target -14.32 dB raw=31 read back raw=31";
 * read_result < 0: "read back failed (-19)". */
std::string volume_set_log_text(const std::string &out, const OutputVolumeModel &m, int level, long raw, int read_result,
                                long read_raw);
/* A model in one line for the log: "-45.00..0.00 dB, 23 levels of 2.05 dB" (or "23 raw steps (no dB data)"). */
std::string volume_model_text(const OutputVolumeModel &m);

}  // namespace cassette
