/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl volume: the level of the output Vinyl plays on (decision D11 as the owner changed it on 2026-11-12). The Vol chip,
 * the volume strip and the - / = keys set the volume of that output and the chip shows its real level:
 *
 *   a sink of the sound server   its own volume (pa_context_set_sink_volume_by_name), in the server's own percent (the
 *                                cubic scale pactl and wpctl show); Vinyl's stream stays at 100 %;
 *   an ALSA card (no server)     its playback mixer control ("Master", "PCM", "Speaker", "Headphone", "Headset", else any
 *                                "... Playback Volume"), on the sound server's own curve where the driver reports dB
 *                                (the coordinator, 2026-11-13: the same percent is the same loudness on every board):
 *                                dB = 60 log10(p / 100), so 50 % = -18.06 dB, 40 % = -23.88 dB, 25 % = -36.12 dB, 100 % =
 *                                0 dB; clamped to the control's range, never above 0 dB; the chip shows what the control
 *                                holds (p = 100 * 10^(dB / 60)). A control without dB data is linear in steps. The
 *                                control's "... Playback Switch" mutes at 0 %;
 *   a card without a mixer       Vinyl's software gain on the same curve (amplitude (p / 100)^3), kept per output
 *                                (output_volume_key) in state.json.
 *
 * The pure helpers below are tested with a fake mixer; the controller (vinyl_app.cpp) reads the sink from the server's
 * lists (refreshed on its change events, app/sink_watch.hpp) and the mixer every 300 ms. LVGL-free.
 */
#pragma once

#include "../alsa/alsa_api.hpp"

#include <string>
#include <vector>

namespace vinyl {

/* The playback volume control Vinyl uses on a card. */
struct MixerChoice {
    std::string volume;      /* "Headset Playback Volume" */
    std::string sw;          /* "Headset Playback Switch", "" when the card has none */
    long min = 0, max = 0;   /* raw range */
    long top = 0;            /* the highest raw value Vinyl sets: max, or the step at 0 dB when the control goes above */
    bool has_db = false;     /* the driver reports dB (percent is then linear in dB) */
    long db_min = 0, db_max = 0; /* hundredths of a dB of min and max, when has_db */
    long db_top = 0;         /* min(db_max, 0) */
    bool min_is_mute = false; /* the control's lowest value is mute: the lowest sound is min + 1 */
    bool ok() const { return !volume.empty() && max > min; }
};

/* The playback control of a card's control list (none: the card has no playback mixer). */
bool choose_playback_mixer(const std::vector<CtlInfo> &ctls, MixerChoice &out);
/* The curve of the sound server (and of Vinyl everywhere): a percent's dB in hundredths, 60 log10(p / 100) (0 % = a
 * very large negative value: the control's minimum), and back, rounded. */
long volume_cdb_for(int pct);
int volume_pct_for(long cdb);
/* The raw value for a percent (0..100), never above `top`: on the curve where the control reports dB (clamped to its
 * minimum), else linear in steps. */
long mixer_raw_for(const MixerChoice &m, int pct);
/* The percent a raw value stands for, on the same curve; top and above (0 dB and over, set by something else) read
 * 100, the value that is mute (min_is_mute) reads 0. */
int mixer_pct_for(const MixerChoice &m, long raw);
/* The level a step of the strip or the keys sets: kVolumeStep further, or 0 (the switch mutes) when a step down would
 * not move the control any more (its minimum already holds: a range that starts above the curve's level there). */
int mixer_step_target(const MixerChoice &m, int current_pct, int delta);
/* The dB of a raw value in hundredths (has_db only). */
long mixer_cdb_for(const MixerChoice &m, long raw);

/* "plughw:CARD=UC02,DEV=0" -> "hw:CARD=UC02"; "default" (and anything else) -> "default" (the default card's mixer). */
std::string mixer_card_for(const std::string &alsa_device);

struct MixerLevel {
    bool has_mixer = false; /* the card has a playback mixer control */
    int pct = -1;
    bool muted = false;
    long cdb = 0;           /* its dB in hundredths (has_db) */
    long raw = 0;           /* the control's value */
};

/* Finds the card's playback control (cached in `choice` for `card`; a different card is looked up again) and reads it.
 * 0, or a negative error (no ALSA, no such card): `out.has_mixer` false then. */
int read_mixer(AlsaApi &alsa, const std::string &card, std::string &choice_card, MixerChoice &choice, MixerLevel &out);
/* Sets the control to `pct` (all channels) and its switch (off at 0 %, on above). 0 or a negative error. */
int write_mixer(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int pct);
/* Sets only the control's switch (mute / unmute), the level kept. -ENOENT without a switch. */
int write_mixer_switch(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, bool on);
/* The log line of a set (the Pi 4B probe, 2026-11-13): "set mixer hw:CARD=Headphones 'PCM Playback Volume' 5 % raw=-7806
 * (target -78.06 dB) read back raw=-7806"; read_result < 0: "read back failed (-19)". read_raw comes from a fresh read of
 * the control (AlsaApi::mixer_get opens the card again). */
std::string mixer_set_log_text(const std::string &card, const MixerChoice &m, int pct, long raw, int read_result, long read_raw);

/* What one press of "-" (delta < 0) or "+" does (the owner's report from the deck, 2026-11-13: "zero7 is showing muted
 * but volume bar shows 18"): while muted "-" does nothing and "+" brings the sound back at the level it had (one step
 * above it when that level is the card's floor, so something is heard); not muted, a step, and "-" at the floor mutes
 * (the card's switch; a card without one stays at its floor). */
struct VolumePress {
    enum class Kind { None, Set, Mute, Unmute } kind = Kind::None;
    int pct = 0; /* Set: the level */
};
VolumePress mixer_press(const MixerChoice &m, const MixerLevel &lv, int delta);
/* The same for a sink of the sound server: its floor is 0 % (no mute flag needed going down); "+" while it is muted (by
 * anything) un-mutes it at its level (0 %: one step). */
VolumePress sink_press(int level, bool muted, int delta);

/* The chip's text for a level: "Vol 45", "Muted", and "Vol --" when the level is not known (negative: the sink is gone,
 * not read yet, or the server reports none): a negative value is never shown or logged (board test B18, 2026-11-13). */
std::string volume_chip_text(int level, bool muted);
/* The level as the log writes it: "45 %", or "-- (not known)". */
std::string volume_log_text(int level);

/* The step the strip and the keys move the level by (percent). */
constexpr int kVolumeStep = 5;
/* The next level after a step: 0..100, never above 100 by a step up (a level someone set above 100 only goes down). */
int volume_after_step(int current, int delta);

}  // namespace vinyl
