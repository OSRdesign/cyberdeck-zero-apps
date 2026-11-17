/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl's equalizer (see eq.hpp).
 */
#include "eq.hpp"

#include <algorithm>
#include <cmath>

namespace vinyl {

const double kEqFreqs[kEqBands] = {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

const char *eq_band_label(int band) {
    static const char *const l[kEqBands] = {"31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
    return band >= 0 && band < kEqBands ? l[band] : "";
}

std::string eq_band_name(int band) {
    static const char *const n[kEqBands] = {"31 Hz", "62 Hz", "125 Hz", "250 Hz", "500 Hz",
                                            "1 kHz", "2 kHz", "4 kHz",  "8 kHz",  "16 kHz"};
    return band >= 0 && band < kEqBands ? n[band] : "";
}

std::string eq_db_text(int db) {
    if (db == 0) return "0 dB";
    return (db > 0 ? "+" : "") + std::to_string(db) + " dB";
}

const char *eq_preset_name(EqPreset p) {
    switch (p) {
    case EqPreset::BassBoost: return "bass_boost";
    case EqPreset::BassCut: return "bass_cut";
    case EqPreset::TrebleBoost: return "treble_boost";
    case EqPreset::Vocal: return "vocal";
    case EqPreset::Loudness: return "loudness";
    case EqPreset::Rock: return "rock";
    case EqPreset::Pop: return "pop";
    case EqPreset::Jazz: return "jazz";
    case EqPreset::Classical: return "classical";
    case EqPreset::Custom: return "custom";
    default: return "flat";
    }
}

const char *eq_preset_word(EqPreset p) {
    switch (p) {
    case EqPreset::BassBoost: return "Bass boost";
    case EqPreset::BassCut: return "Bass cut";
    case EqPreset::TrebleBoost: return "Treble boost";
    case EqPreset::Vocal: return "Vocal";
    case EqPreset::Loudness: return "Loudness";
    case EqPreset::Rock: return "Rock";
    case EqPreset::Pop: return "Pop";
    case EqPreset::Jazz: return "Jazz";
    case EqPreset::Classical: return "Classical";
    case EqPreset::Custom: return "Custom";
    default: return "Flat";
    }
}

EqPreset eq_preset_from(const std::string &s, EqPreset fallback) {
    for (EqPreset p : kEqPresets)
        if (s == eq_preset_name(p)) return p;
    return fallback;
}

EqGains eq_preset_gains(EqPreset p) {
    /* 31, 62, 125, 250, 500, 1k, 2k, 4k, 8k, 16k (the builder's values: gentle curves, nothing above +6 dB) */
    switch (p) {
    case EqPreset::BassBoost: return {6, 5, 4, 2, 0, 0, 0, 0, 0, 0};
    case EqPreset::BassCut: return {-6, -5, -4, -2, 0, 0, 0, 0, 0, 0};
    case EqPreset::TrebleBoost: return {0, 0, 0, 0, 0, 0, 2, 4, 5, 6};
    case EqPreset::Vocal: return {-2, -2, -1, 0, 2, 4, 4, 2, 0, -1};
    case EqPreset::Loudness: return {5, 4, 2, 0, -1, -1, 0, 2, 4, 5};
    case EqPreset::Rock: return {4, 3, 2, 0, -1, -1, 1, 2, 3, 4};
    case EqPreset::Pop: return {-1, 1, 3, 4, 3, 1, -1, -1, -1, -1};
    case EqPreset::Jazz: return {3, 2, 1, 2, -1, -1, 0, 1, 2, 3};
    case EqPreset::Classical: return {4, 3, 2, 1, 0, 0, 0, 2, 3, 4};
    default: return EqGains{};
    }
}

EqGains EqSettings::gains() const { return preset == EqPreset::Custom ? custom : eq_preset_gains(preset); }

EqGains EqSettings::effective() const { return on ? gains() : EqGains{}; }

bool EqSettings::bypass() const {
    const EqGains g = effective();
    return std::all_of(g.begin(), g.end(), [](int v) { return v == 0; });
}

void EqSettings::set_band(int band, int db) {
    if (band < 0 || band >= kEqBands) return;
    if (preset != EqPreset::Custom) {
        custom = gains();
        preset = EqPreset::Custom;
    }
    custom[static_cast<size_t>(band)] = std::max(-kEqMaxDb, std::min(kEqMaxDb, db));
}

std::string eq_menu_value(const EqSettings &e) { return e.on ? eq_preset_word(e.preset) : "Off"; }

double eq_max_gain_db(const EqGains &g, double fs) {
    if (!(fs > 0)) return 0;
    BiquadCoef c[kEqBands];
    int n = 0;
    for (int i = 0; i < kEqBands; ++i)
        if (g[static_cast<size_t>(i)] != 0 && kEqFreqs[i] < 0.45 * fs) c[n++] = biquad_peaking(fs, kEqFreqs[i], kEqQ, g[static_cast<size_t>(i)]);
    if (n == 0) return 0;
    double best = 0;
    auto at = [&](double f) {
        double db = 0;
        for (int k = 0; k < n; ++k) db += biquad_response_db(c[k], f, fs);
        best = std::max(best, db);
    };
    const double hi = std::min(20000.0, 0.45 * fs);
    constexpr int kGrid = 192;
    for (int k = 0; k < kGrid; ++k) at(20.0 * std::pow(hi / 20.0, static_cast<double>(k) / (kGrid - 1)));
    for (int i = 0; i < kEqBands; ++i)
        if (kEqFreqs[i] < 0.45 * fs) at(kEqFreqs[i]);
    return best;
}

void Equalizer::configure(const EqGains &g, uint32_t rate) {
    if (configured_ && g == g_ && rate == rate_) return;
    const bool was_working = nb_ > 0;
    std::array<Band, kEqBands> old = b_;
    const int old_n = nb_;
    nb_ = 0;
    for (int i = 0; i < kEqBands; ++i) {
        const int db = g[static_cast<size_t>(i)];
        if (db == 0 || rate == 0 || kEqFreqs[i] >= 0.45 * rate) continue;
        Band b;
        b.idx = i;
        b.c = biquad_peaking(rate, kEqFreqs[i], kEqQ, db);
        if (rate == rate_)
            for (int k = 0; k < old_n; ++k)
                if (old[static_cast<size_t>(k)].idx == i) b.s = old[static_cast<size_t>(k)].s; /* a band that stays keeps its state */
        b_[static_cast<size_t>(nb_++)] = b;
    }
    g_ = g;
    rate_ = rate;
    configured_ = true;
    pre_target_ = nb_ > 0 ? static_cast<float>(std::pow(10.0, -eq_max_gain_db(g, rate) / 20.0)) : 1.0f;
    if (!was_working && pre_ == 1.0f) pre_ = pre_target_; /* it starts working: the headroom at once (no ramp from 0 dB) */
}

void Equalizer::process(float *io, size_t frames) {
    if (frames == 0) return;
    if (pre_ != pre_target_) { /* the headroom moves over this block */
        const float a = pre_, d = (pre_target_ - pre_) / static_cast<float>(frames);
        for (size_t f = 0; f < frames; ++f) {
            const float k = a + d * static_cast<float>(f + 1);
            io[2 * f] *= k;
            io[2 * f + 1] *= k;
        }
        pre_ = pre_target_;
    } else if (pre_ != 1.0f) {
        for (size_t i = 0; i < 2 * frames; ++i) io[i] *= pre_;
    }
    for (int k = 0; k < nb_; ++k) biquad_process(b_[static_cast<size_t>(k)].c, b_[static_cast<size_t>(k)].s, io, frames);
}

void Equalizer::reset() {
    for (int k = 0; k < nb_; ++k) b_[static_cast<size_t>(k)].s.clear();
    pre_ = pre_target_;
}

double Equalizer::headroom_db() const { return 20.0 * std::log10(std::max(1e-6f, pre_target_)); }

}  // namespace vinyl
