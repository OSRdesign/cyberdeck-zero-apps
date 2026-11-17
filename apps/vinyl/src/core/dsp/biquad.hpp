/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl P2d (report 060 3b): one biquad filter of the equalizer, the RBJ "Audio EQ Cookbook" peaking filter (Robert
 * Bristow-Johnson's formulas, public), in transposed direct form II with double coefficients and double state (the 31 Hz
 * band's poles sit close to the unit circle, where a float state adds noise), float samples in and out, interleaved
 * stereo. LVGL-free.
 */
#pragma once

#include <cstddef>

namespace vinyl {

/* normalised by a0: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 */
struct BiquadCoef {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

/* The peaking filter at f0 Hz (quality q, gain_db at f0, 0 dB far from it) for the sample rate fs. */
BiquadCoef biquad_peaking(double fs, double f0, double q, double gain_db);
/* |H| in dB at f Hz (fs the sample rate). */
double biquad_response_db(const BiquadCoef &c, double f, double fs);

/* the two state words of each channel (transposed direct form II) */
struct BiquadState {
    double s1l = 0, s2l = 0, s1r = 0, s2r = 0;
    void clear() { s1l = s2l = s1r = s2r = 0; }
};

/* Filters `frames` interleaved stereo frames in place. A state that decayed under 1e-30 is set to 0 (no denormals after
 * a long silence). */
void biquad_process(const BiquadCoef &c, BiquadState &st, float *io, size_t frames);

}  // namespace vinyl
