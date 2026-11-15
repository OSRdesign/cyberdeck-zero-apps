/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the Huffman tables of a camera frame (brief 7 step 2, plan 16 C5). An MJPEG camera may leave the DHT segment out
 * of its frames (the "AVI1" MJPEG convention: every decoder assumes the typical tables of ITU-T T.81 Annex K.3); a JPEG
 * file must carry them. ensure_dht() inserts those four tables (DC and AC, luminance and chrominance) as one DHT segment
 * right before the first SOS when the frame has no DHT; a frame with its own tables is copied unchanged (the FineCam's
 * frames carry them, P0-a; the C270's may not). Pure, LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vhs {

/* The DHT segment of Annex K.3: FF C4, its length, then the tables 0x00 (DC 0), 0x10 (AC 0), 0x01 (DC 1), 0x11 (AC 1). */
const std::vector<std::uint8_t> &annex_k_dht();

/* out = the frame with the Annex K DHT before its first SOS when it has no DHT, else the frame as it is. False when the
 * frame is not a JPEG this can walk (no SOI, a broken segment, no SOS). inserted (may be null): whether tables went in. */
bool ensure_dht(const std::uint8_t *jpeg, std::size_t n, std::vector<std::uint8_t> &out, bool *inserted);

} // namespace vhs
