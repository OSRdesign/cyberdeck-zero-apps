/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the card's own capture controls, found by name and only when they exist (a card without a level control must
 * work: has_level is false and the UI hides the CARD stepper and the Settings rows).
 * Names seen in P0: "Mic Capture Volume", "Mic Capture Switch", "Auto Gain Control". Reading is the default: nothing is changed
 * unless a set_* function or reapply() is called.
 *
 * dB: the level is shown in dB. When the driver reports a dB range it is used (linear between min and max: an assumption for
 * cards whose steps are not evenly spaced). When it reports none (the UC02 headset: the kernel disables "bogus dB values"),
 * the range is INFERRED as 0 .. 23.81 dB over the raw range: both measured cards (PCM2902 raw 0-16, UC02 raw 0-127) spanned
 * 0 to +23.81 dB. LevelRange::db_inferred says which case applies.
 */
#pragma once

#include <string>
#include <vector>

#include "alsa_api.hpp"

namespace cassette {

constexpr double kInferredDbMin = 0.0;
constexpr double kInferredDbMax = 23.81;

struct LevelRange {
    long raw_min = 0, raw_max = 0, step = 1;
    double db_min = 0, db_max = 0;
    bool db_inferred = false; /* true: no dB from the driver, the range above is the 0..23.81 dB assumption */
    double raw_to_db(long raw) const;
    long db_to_raw(double db) const; /* clamped to the range and rounded to the nearest step */
};

struct MixerCaps {
    bool has_level = false;
    bool has_agc = false;
    bool has_mute = false;
};

struct MixerState {
    bool level_valid = false;
    long level_raw = 0;
    double level_db = 0;
    bool agc_valid = false;
    bool agc = false;
    bool mute_valid = false;
    bool mute = false; /* capture switch off */
};

/* What to reapply at every start (the mixer resets at reboot). Only the parts with set_* true are written. */
struct MixerSetting {
    bool set_level = false;
    double level_db = 0;
    bool set_agc = false;
    bool agc = false;
    bool set_mute = false;
    bool mute = false;
};

class Mixer {
public:
    /* card is the mixer device, for example "hw:CARD=Device" (card.hpp: mixer_device). */
    Mixer(AlsaApi &api, std::string card) : api_(api), card_(std::move(card)) {}

    /* Lists the card's controls and picks the capture level, capture switch and auto gain. Returns the mixer_list error, or 0
     * (also when the card has none of them). */
    int discover();
    const MixerCaps &caps() const { return caps_; }
    const LevelRange &level_range() const { return range_; }
    const std::string &level_name() const { return level_name_; }
    const std::string &mute_name() const { return mute_name_; }
    const std::string &agc_name() const { return agc_name_; }
    const std::vector<CtlInfo> &controls() const { return ctls_; }

    /* Reads whatever exists. Never writes. */
    MixerState read();
    /* Sets the level to the nearest step to db (clamped); returns the dB actually set, or a negative error in `err`. */
    bool set_level_db(double db, double *applied_db = nullptr, int *err = nullptr);
    bool set_agc(bool on, int *err = nullptr);
    bool set_mute(bool muted, int *err = nullptr);
    /* Writes the saved choices for the controls that exist; returns how many writes succeeded. */
    int reapply(const MixerSetting &s);

private:
    AlsaApi &api_;
    std::string card_;
    std::vector<CtlInfo> ctls_;
    MixerCaps caps_;
    LevelRange range_;
    std::string level_name_, mute_name_, agc_name_;
};

} // namespace cassette
