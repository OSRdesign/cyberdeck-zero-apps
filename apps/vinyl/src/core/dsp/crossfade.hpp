/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl P2d: the crossfade's arithmetic (report 060 3c; owner decision D29 of 2026-11-13: offered on every board, Off by
 * default, never inside an album, only between tracks of the same format). The engine (core/engine) decides when and runs
 * the two decoders; this file is the length and the mix, pure and tested:
 *
 *   setting   Off, 2, 4, 6, 8 or 10 seconds (settings.json "crossfade": the seconds, 0 = Off)
 *   length    the setting, never more than half of either track (a track shorter than twice the fade fades for half its
 *             length), never more than what is left of the outgoing track; under half a second there is no fade (the
 *             gapless change of P1)
 *   curves    equal power: the outgoing track at cos(pi/2 t), the incoming one at sin(pi/2 t), t = (i + 0.5) / length;
 *             cos^2 + sin^2 = 1, so two unrelated tracks keep their loudness through the fade (two equal full-scale
 *             signals can reach 1.41 at the middle: the engine's limiter takes that)
 *
 * LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vinyl {

constexpr int kCrossfadeChoices[] = {0, 2, 4, 6, 8, 10};
constexpr int kCrossfadeMaxS = 10;
constexpr uint32_t kCrossfadeMinMs = 500;

/* "Off", "2 s" ... "10 s" */
std::string crossfade_word(int seconds);
/* a stored value: one of the choices (anything else is Off) */
int crossfade_valid(int seconds);

/* The fade's length in frames for tracks of total_a and total_b frames (0: not known; a track of unknown length gives no
 * limit, the engine needs total_a), 0 when there is no fade (Off, or under kCrossfadeMinMs). */
uint64_t crossfade_frames(int seconds, uint32_t rate, uint64_t total_a, uint64_t total_b);
/* the equal-power gains at frame i of a fade of len frames (i >= len: 0 and 1) */
void crossfade_gains(uint64_t i, uint64_t len, double &g_out, double &g_in);
/* dst = out x g_out + in x g_in for `frames` interleaved stereo frames from frame `pos` of the fade (dst may be one of
 * the inputs) */
void crossfade_mix(const float *out_track, const float *in_track, float *dst, size_t frames, uint64_t pos, uint64_t len);

}  // namespace vinyl
