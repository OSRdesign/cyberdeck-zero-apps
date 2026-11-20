/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl P2d: the equalizer (report 060 3b; owner decision D30 of 2026-11-13: one EQ for every output; the canvas boards
 * EQ-480x320 and EQ-320x480):
 *
 *   bands      10, the ISO octave centres 31 Hz ... 16 kHz, RBJ peaking filters of quality 1.41 (core/dsp/biquad)
 *   gains      -12 to +12 dB in 1 dB steps; a band at 0 dB is not computed; all at 0, or the EQ Off, is a bypass (the
 *              engine keeps its integer path, bit-exact)
 *   no pre-gain a boosted band rises by its gain and every other band stays at 0 dB (the owner's board test of the 0.7.0
 *              draft, 2026-11-20: the first draft lowered the whole signal by the curve's highest point, so a boost made
 *              the rest quieter); the engine's look-ahead limiter (core/dsp/limiter, ceiling -1 dBFS) is after the EQ
 *              whenever the EQ works and takes the peaks a boost pushes over its ceiling (ReplayGain Track, the default,
 *              usually leaves several dB of room before it)
 *   presets    Flat, Bass boost, Bass cut, Treble boost, Vocal, Loudness, Rock, Pop, Jazz, Classical, Custom (the
 *              builder's values, report 060 3b names them [I]); moving a band makes the curve Custom
 *   cost       10 biquads x 2 channels per frame at most, double state: about 1 % of one Pi Zero 2 W core at 44.1 kHz
 *              (vinyl --bench-dsp measures it)
 *
 * LVGL-free.
 */
#pragma once

#include "biquad.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace vinyl {

constexpr int kEqBands = 10;
constexpr int kEqMaxDb = 12;
constexpr double kEqQ = 1.41;
/* the centres in Hz: 31.25, 62.5, 125 ... 16000 */
extern const double kEqFreqs[kEqBands];
/* the bars' labels: "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" */
const char *eq_band_label(int band);
/* the band line's name: "31 Hz" ... "500 Hz", "1 kHz" ... "16 kHz" */
std::string eq_band_name(int band);
/* "+3 dB", "0 dB", "-12 dB" */
std::string eq_db_text(int db);

using EqGains = std::array<int, kEqBands>;

enum class EqPreset { Flat, BassBoost, BassCut, TrebleBoost, Vocal, Loudness, Rock, Pop, Jazz, Classical, Custom };
constexpr EqPreset kEqPresets[] = {EqPreset::Flat,     EqPreset::BassBoost, EqPreset::BassCut, EqPreset::TrebleBoost,
                                   EqPreset::Vocal,    EqPreset::Loudness,  EqPreset::Rock,    EqPreset::Pop,
                                   EqPreset::Jazz,     EqPreset::Classical, EqPreset::Custom};
/* "flat", "bass_boost" ... "custom" (the settings file, the pick list's ids) */
const char *eq_preset_name(EqPreset p);
/* "Flat", "Bass boost" ... "Custom" (the pages) */
const char *eq_preset_word(EqPreset p);
EqPreset eq_preset_from(const std::string &s, EqPreset fallback);
/* a preset's gains (Custom: all 0, the caller keeps its own) */
EqGains eq_preset_gains(EqPreset p);

/* The setting (settings.json "eq": {"on", "preset", "custom"}). On by default with the preset Flat: a bypass until a
 * preset or a band is chosen. */
struct EqSettings {
    bool on = true;
    EqPreset preset = EqPreset::Flat;
    EqGains custom{}; /* the Custom curve, kept while another preset is chosen */
    /* the curve chosen (the preset's, or the custom one) */
    EqGains gains() const;
    /* what the engine applies: the curve when On, else all 0 */
    EqGains effective() const;
    bool bypass() const;
    /* a band moved (clamped to -12..+12): the curve is Custom from now on, starting from the preset's other bands */
    void set_band(int band, int db);
    /* Reset: the preset Flat (the Custom curve kept: choosing Custom brings it back) */
    void reset() { preset = EqPreset::Flat; }
    bool operator==(const EqSettings &o) const { return on == o.on && preset == o.preset && custom == o.custom; }
    bool operator!=(const EqSettings &o) const { return !(*this == o); }
};
/* the Menu row's value: "Off", else the preset's word ("Flat", "Bass boost", "Custom") */
std::string eq_menu_value(const EqSettings &e);

/* The highest point of the curve in dB (0 when nothing rises above 0 dB): the bands' responses added, read on a log grid
 * from 20 Hz to min(20 kHz, 0.45 fs) and at the centres. Bands at or above 0.45 fs are left out (as the filter does).
 * Information only (the journal line): nothing lowers the signal by it. */
double eq_max_gain_db(const EqGains &g, double fs);

/* The filters of a curve at a sample rate, run by the engine thread on float interleaved stereo. */
class Equalizer {
public:
    /* New gains or a new rate: the coefficients again; a band that stays keeps its state, a new one starts at rest. */
    void configure(const EqGains &g, uint32_t rate);
    /* something to do: a band that is not 0 dB */
    bool active() const { return nb_ > 0; }
    /* the bands' filters, nothing else (no gain before or after them) */
    void process(float *io, size_t frames);
    /* a new stream (a seek, a new output): the states at rest */
    void reset();
    int bands() const { return nb_; }
    const EqGains &gains() const { return g_; }

private:
    struct Band {
        int idx = -1;
        BiquadCoef c;
        BiquadState s;
    };
    std::array<Band, kEqBands> b_{};
    int nb_ = 0;
    EqGains g_{};
    uint32_t rate_ = 0;
    bool configured_ = false;
};

}  // namespace vinyl
