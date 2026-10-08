/*
 * SPDX-License-Identifier: MIT
 */

#include "presets.hpp"

#include <cmath>
#include <cstdio>

namespace meshzero {

namespace {
struct Raw {
    const char *name;
    double freq, bw;
    int sf, cr;
};

const Raw kRaw[] = {
#include "radio_presets.inc"
};
} // namespace

const std::vector<RadioPreset> &radio_presets()
{
    static const std::vector<RadioPreset> v = [] {
        std::vector<RadioPreset> out;
        for (const Raw &r : kRaw) out.push_back({r.name, r.freq, r.bw, r.sf, r.cr});
        return out;
    }();
    return v;
}

const char *presets_source_url()
{
    return MESHHOP_PRESETS_SOURCE_URL;
}

const char *presets_date()
{
    return MESHHOP_PRESETS_DATE;
}

std::string preset_detail(const RadioPreset &p)
{
    char b[96];
    std::snprintf(b, sizeof(b), "%.3f MHz  %g kHz  SF%d  4/%d", p.freq_mhz, p.bw_khz, p.sf, p.cr);
    return b;
}

int find_preset(double freq_mhz, double bw_khz, int sf, int cr)
{
    const auto &v = radio_presets();
    for (size_t i = 0; i < v.size(); ++i) {
        const RadioPreset &p = v[i];
        if (std::fabs(p.freq_mhz - freq_mhz) < 0.0005 && std::fabs(p.bw_khz - bw_khz) < 0.05 && p.sf == sf && p.cr == cr)
            return static_cast<int>(i);
    }
    return -1;
}

bool apply_preset(RadioSettings &s, int index)
{
    const auto &v = radio_presets();
    if (index < 0 || index >= static_cast<int>(v.size())) return false;
    const RadioPreset &p = v[static_cast<size_t>(index)];
    s.freq_mhz = std::round(p.freq_mhz * 1000.0) / 1000.0;
    s.bw_khz = p.bw_khz;
    s.sf = p.sf;
    s.cr = p.cr;
    return true;
}

const std::vector<int> &spreading_factors()
{
    static const std::vector<int> v = {5, 6, 7, 8, 9, 10, 11, 12};
    return v;
}

const std::vector<int> &coding_rates()
{
    static const std::vector<int> v = {5, 6, 7, 8};
    return v;
}

std::string fmt_bw_label(double khz)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%g kHz", khz);
    return b;
}

std::string fmt_cr_label(int cr)
{
    return "4/" + std::to_string(cr);
}

std::string fmt_sf_label(int sf)
{
    return "SF" + std::to_string(sf);
}

int bandwidth_index(double khz)
{
    const auto &v = lora_bandwidths();
    int best = 0;
    double best_d = 1e9;
    for (size_t i = 0; i < v.size(); ++i) {
        const double d = std::fabs(v[i] - khz);
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int sf_index(int sf)
{
    const auto &v = spreading_factors();
    int best = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::abs(v[i] - sf) < std::abs(v[static_cast<size_t>(best)] - sf)) best = static_cast<int>(i);
    return best;
}

int cr_index(int cr)
{
    const auto &v = coding_rates();
    int best = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::abs(v[i] - cr) < std::abs(v[static_cast<size_t>(best)] - cr)) best = static_cast<int>(i);
    return best;
}

} // namespace meshzero
