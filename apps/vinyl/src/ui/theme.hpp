/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the colours of the owner-approved drafts (docs/design, 2026-11-12), named once.
 */
#pragma once

#include <cstdint>

namespace vinyl {
namespace ui {
namespace col {

constexpr std::uint32_t kScreen = 0x000000;
constexpr std::uint32_t kCream = 0xDCD4BD;    /* the panel, the mini player, the notices */
constexpr std::uint32_t kButton = 0xB9B19B;   /* buttons on cream */
constexpr std::uint32_t kChip = 0xC9C0A5;     /* chips, the seek track, the up-next box */
constexpr std::uint32_t kInfoChip = 0xE6DFC8; /* the flat information chips of the 640x480 draft (format, Gapless) */
constexpr std::uint32_t kInk = 0x222222;      /* titles, icons */
constexpr std::uint32_t kInk2 = 0x3B3828;     /* artist, remaining time, notice text */
constexpr std::uint32_t kInk3 = 0x6B6552;     /* album, the mini player's second line, UP NEXT */
constexpr std::uint32_t kHeaderChip = 0x222222;
constexpr std::uint32_t kHeaderText = 0xEEEEEE;
constexpr std::uint32_t kWhite = 0xFFFFFF;
constexpr std::uint32_t kTeal = 0x1F7A72;     /* the accent: play, volume chip, seek fill, label */
constexpr std::uint32_t kMutedFill = 0x222222; /* the level box while muted, its mark and word white (the coordinator's
                                                  pick, 2026-11-13: the same on every layout) */
constexpr std::uint32_t kList = 0x1B1E1E;     /* the dark list panel */
constexpr std::uint32_t kRowSel = 0x2A2A2A;   /* the selected row, the A-Z chip */
constexpr std::uint32_t kRowText = 0xDDDDDD;
constexpr std::uint32_t kRowValue = 0x9A9A9A; /* durations, values, headings, chevrons */
constexpr std::uint32_t kRowDim = 0x7A7A7A;   /* a paired device that is not connected */
constexpr std::uint32_t kDotDim = 0x555555;
constexpr std::uint32_t kPlaying = 0x3B82F6;  /* the dot of the playing track / the chosen output */
constexpr std::uint32_t kDanger = 0x8A2A22;   /* Clear all, the disconnect notice */
constexpr std::uint32_t kGroove1 = 0x141414, kGroove2 = 0x1F1F1F;

} // namespace col
} // namespace ui
} // namespace vinyl
