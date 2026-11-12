/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl volume: the level of the output Vinyl plays on (decision D11 as the owner changed it on 2026-11-12). The Vol chip,
 * the volume strip and the - / = keys set the volume of that output and the chip shows its real level:
 *
 *   a sink of the sound server   its own volume (pa_context_set_sink_volume_by_name), in the server's own percent (the
 *                                cubic scale pactl and wpctl show); Vinyl's stream stays at 100 %;
 *   an ALSA card (no server)     its playback mixer control ("Master", "PCM", "Speaker", "Headphone", "Headset", else any
 *                                "... Playback Volume"): percent linear in dB where the driver reports dB, else linear in
 *                                steps; never above 0 dB; the control's "... Playback Switch" mutes at 0 %;
 *   a card without a mixer       Vinyl's software gain, kept per output (output_volume_key) in state.json.
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
    bool ok() const { return !volume.empty() && max > min; }
};

/* The playback control of a card's control list (none: the card has no playback mixer). */
bool choose_playback_mixer(const std::vector<CtlInfo> &ctls, MixerChoice &out);
/* The raw value for a percent (0..100), never above `top`. */
long mixer_raw_for(const MixerChoice &m, int pct);
/* The percent of a raw value (0 at min, 100 at top; above top it reports more than 100). */
int mixer_pct_for(const MixerChoice &m, long raw);
/* The dB of a raw value in hundredths (has_db only). */
long mixer_cdb_for(const MixerChoice &m, long raw);

/* "plughw:CARD=UC02,DEV=0" -> "hw:CARD=UC02"; "default" (and anything else) -> "default" (the default card's mixer). */
std::string mixer_card_for(const std::string &alsa_device);

struct MixerLevel {
    bool has_mixer = false; /* the card has a playback mixer control */
    int pct = -1;
    bool muted = false;
    long cdb = 0;           /* its dB in hundredths (has_db) */
};

/* Finds the card's playback control (cached in `choice` for `card`; a different card is looked up again) and reads it.
 * 0, or a negative error (no ALSA, no such card): `out.has_mixer` false then. */
int read_mixer(AlsaApi &alsa, const std::string &card, std::string &choice_card, MixerChoice &choice, MixerLevel &out);
/* Sets the control to `pct` (all channels) and its switch (off at 0 %, on above). 0 or a negative error. */
int write_mixer(AlsaApi &alsa, const std::string &card, const MixerChoice &choice, int pct);

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
