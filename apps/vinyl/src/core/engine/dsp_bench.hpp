/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl P2d: `vinyl --bench-dsp` (report 060 2.5, board test B87): the CPU of the float chain on `seconds` of synthetic
 * 44.1 kHz stereo audio (16-bit samples in, 16-bit out), measured on this thread's CPU clock, part by part:
 *
 *   float path   int to float, the gain, the limiter, back to int (what a boost, the EQ or the crossfade turns on)
 *   EQ           the equalizer with every band at work (the worst case: 10 biquads x 2 channels)
 *   crossfade    a second track converted and mixed (during a fade only; the second decoder is not counted: report 059
 *                measured it, FLAC +0.65 %, MP3 +0.8 %, Vorbis +2.1 %, Opus +2.6 to 3.6 % on a Zero 2 W)
 *
 * Each is given as a percent of one core (CPU seconds / audio seconds x 100). LVGL-free; the unit tests run it short.
 */
#pragma once

#include <string>

namespace vinyl {

struct DspBench {
    double seconds = 0;   /* audio benchmarked */
    double base_pct = 0;  /* the float path with the limiter */
    double eq_pct = 0;    /* the 10-band EQ alone */
    double fade_pct = 0;  /* the crossfade's mix and second conversion alone */
    double total_pct = 0; /* all of it, as in a fade with the EQ on */
};

DspBench run_dsp_bench(double seconds);
std::string dsp_bench_text(const DspBench &b);

}  // namespace vinyl
