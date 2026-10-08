/*
 * SPDX-License-Identifier: MIT
 *
 * The radio presets of the MeshCore firmware (data file radio_presets.inc: 26 entries, dated, with its source URL) and the
 * fixed choice lists of the Settings screen (bandwidth, spreading factor, coding rate). No LVGL, no I/O.
 */

#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace meshzero {

struct RadioPreset {
    std::string name;            // as listed by the source, e.g. "EU/UK (Deprecated)"
    double freq_mhz = 0;
    double bw_khz = 0;
    int sf = 0;
    int cr = 0;                  // 5..8 = 4/5..4/8
    bool deprecated() const { return name.find("(Deprecated)") != std::string::npos; }
};

/* The list in use: the bundled one at first, then the cached or freshly fetched one (set_presets). Main thread only; the UI swaps
 * it only while no preset popup is open (the popup holds indexes into it). */
const std::vector<RadioPreset> &radio_presets();
const std::vector<RadioPreset> &bundled_presets();
const char *presets_source_url();     // where the list comes from
const char *presets_date();           // when the BUNDLED list was fetched, YYYY-MM-DD

enum class PresetOrigin { Bundled, Cached, Fetched };
/* Replaces the list in use. `date` is the day the list was fetched. */
void set_presets(std::vector<RadioPreset> list, PresetOrigin origin, const std::string &date);
void reset_presets();                  // back to the bundled list (tests)
PresetOrigin presets_origin();
const std::string &presets_list_date();
/* "meshcore.nz list, fetched 2026-10-08" / "saved copy of the meshcore.nz list, fetched ..." / "list built into the app, 2026-10-07" */
std::string presets_origin_text();

/* "869.618 MHz  62.5 kHz  SF8  4/8" */
std::string preset_detail(const RadioPreset &p);
/* The index of the preset equal to these radio parameters (frequency to 1 kHz), or -1 (custom settings). */
int find_preset(double freq_mhz, double bw_khz, int sf, int cr);
/* Copies frequency, bandwidth, SF and CR of preset `index` into s (name and TX power are kept). False for a bad index. */
bool apply_preset(RadioSettings &s, int index);

/* Fixed choices (the popup with the value in the middle). */
const std::vector<int> &spreading_factors();         // 5..12
const std::vector<int> &coding_rates();              // 5..8
std::string fmt_bw_label(double khz);                // "62.5 kHz"
std::string fmt_cr_label(int cr);                    // "4/5"
std::string fmt_sf_label(int sf);                    // "SF7"
/* Index of the bandwidth / SF / CR in its list; the nearest one when it is not listed. */
int bandwidth_index(double khz);
int sf_index(int sf);
int cr_index(int cr);

} // namespace meshzero
