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
 *
 * Mic boost (0.4.1, the Panasonic FZ-M1's Intel HDA ALC256): a card may have a second gain stage in front of the capture level,
 * "<X> Mic Boost Volume" ("Internal Mic Boost Volume", "Headset Mic Boost Volume", or just "Mic Boost Volume"), a few steps of 10 dB.
 * Cassette offers the boost of the ACTIVE input: the one named like the selected item of the card's "Input Source" /
 * "Capture Source" enumeration (or, without one, like the "<X> Capture Switch" that is on); with none known, "Internal ..."
 * first, else the first boost. The active input is looked at again at every read(): the jack can switch it.
 *
 * Sound server (0.4.1 draft.2): where PulseAudio (or pipewire-pulse) owns the card's "Capture" element, a direct write is reset
 * by the server (the FZ-M1's stuck -17.3 dB). With use_server() the capture LEVEL is set on the server's source of the card
 * (server_gain.hpp: pactl, V = base * 10^(G/60)) and read back from the ALSA control, i.e. what reached the hardware; when the
 * server does not take it, the card is written directly as before (level_note() says so). Without use_server(), or without a
 * server, nothing changes: the ALSA path only. The boost, switch and auto gain stay on ALSA.
 */
#pragma once

#include <string>
#include <vector>

#include "alsa_api.hpp"
#include "server_gain.hpp"

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

/* One step of a boost control: the raw value, its gain and the text of the picker ("Off", "+10 dB"). */
struct BoostChoice {
    long raw = 0;
    double db = 0;
    std::string label;
};

struct MixerCaps {
    bool has_boost = false; /* some "... Mic Boost Volume" control exists */
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
    bool boost_valid = false;
    long boost_raw = 0;
    double boost_db = 0; /* 0 when the control gives no dB */
    int boost_index = -1; /* index into Mixer::boost_choices() of boost_raw (-1: a raw value that is none of them) */
};

/* What to reapply at every start (the mixer resets at reboot). Only the parts with set_* true are written. */
struct MixerSetting {
    bool set_level = false;
    double level_db = 0;
    bool set_agc = false;
    bool agc = false;
    bool set_mute = false;
    bool mute = false;
    bool set_boost = false;
    double boost_db = 0; /* nearest step of the active boost control */
};

class Mixer {
public:
    /* card is the mixer device, for example "hw:CARD=Device" (card.hpp: mixer_device). */
    Mixer(AlsaApi &api, std::string card) : api_(api), card_(std::move(card)) {}

    /* Lets discover() look for a sound-server source of ALSA card number `alsa_card` (this boot's index, used at once, never
     * stored). Call before discover(); `runner` must outlive the Mixer. */
    void use_server(PactlRunner *runner, int alsa_card) {
        runner_ = runner;
        alsa_card_ = alsa_card;
    }
    /* The level goes through the server's source (name below, "" when not) */
    bool level_via_server() const { return sg_ && sg_->active() && !server_off_; }
    std::string server_source() const { return sg_ ? sg_->source() : std::string(); }
    /* What the server did with the last level write, for the journal: "asked V 3600, source now V 3381 (-17.25 dB)"; "" when none. */
    const std::string &server_report() const { return report_; }
    /* A sentence about the last level write that did not go the usual way (the server did not take it); "" when none. Cleared by the call. */
    std::string take_note() {
        std::string n;
        n.swap(note_);
        return n;
    }

    /* Lists the card's controls and picks the capture level, capture switch and auto gain. Returns the mixer_list error, or 0
     * (also when the card has none of them). */
    int discover();
    const MixerCaps &caps() const { return caps_; }
    const LevelRange &level_range() const { return range_; }
    const std::string &level_name() const { return level_name_; }
    const std::string &mute_name() const { return mute_name_; }
    const std::string &agc_name() const { return agc_name_; }
    const std::vector<CtlInfo> &controls() const { return ctls_; }
    /* The boost of the active input: its control's name and input ("Internal Mic"; "" for a plain "Mic Boost Volume"), and the
     * steps it offers (the Off / +10 / +20 / +30 dB of the picker). Empty without a boost. Updated by discover() and read(). */
    const std::string &boost_name() const { return boost_name_; }
    const std::string &boost_source() const { return boost_source_; }
    const std::vector<BoostChoice> &boost_choices() const { return boost_choices_; }

    /* Reads whatever exists. Never writes. */
    MixerState read();
    /* Sets the level to the nearest step to db (clamped); returns the dB actually set, or a negative error in `err`. */
    bool set_level_db(double db, double *applied_db = nullptr, int *err = nullptr);
    /* The boost control of the active input to the step nearest db / to choice `index`; the dB actually set in `applied_db`. */
    bool set_boost_db(double db, double *applied_db = nullptr, int *err = nullptr);
    bool set_boost_index(int index, int *err = nullptr);
    bool set_agc(bool on, int *err = nullptr);
    bool set_mute(bool muted, int *err = nullptr);
    /* Writes the saved choices for the controls that exist; returns how many writes succeeded. */
    int reapply(const MixerSetting &s);

private:
    AlsaApi &api_;
    std::string card_;
    PactlRunner *runner_ = nullptr;
    int alsa_card_ = -1;
    std::unique_ptr<ServerGain> sg_;
    bool server_off_ = false; /* the server did not take a level: direct writes from now on */
    std::string note_, report_;
    bool set_level_via_server(long want_raw, double want_db);
    std::vector<CtlInfo> ctls_;
    MixerCaps caps_;
    LevelRange range_;
    std::string level_name_, mute_name_, agc_name_;
    struct Boost {
        std::string name, source; /* "Internal Mic Boost Volume", "Internal Mic" */
        CtlInfo ctl; /* a copy of its listing */
    };
    std::vector<Boost> boosts_;
    int boost_active_ = -1; /* index into boosts_ */
    std::string boost_name_, boost_source_;
    std::vector<BoostChoice> boost_choices_;
    void pick_active_boost(); /* looks at the card's input source and sets boost_active_ and the boost_* members */
};

} // namespace cassette
