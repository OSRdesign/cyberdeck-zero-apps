/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the colours, all from the launcher toolkit's palette (cp0_ui_list.h, cp0_ui_osk.h): the three target
 * colours are the accent blue, the warn amber and the light key grey, told apart by every kind of colour vision (blue,
 * yellow and near white differ in hue and in lightness), and each target also carries its name (T1..T3) everywhere.
 */
#pragma once

#include <cstdint>

namespace mmwui {
namespace pal {
constexpr std::uint32_t kTarget[3] = {0x5AAAFF, 0xF0B400, 0xE8E8E8}; /* T1 blue, T2 amber, T3 light grey */
constexpr std::uint32_t kText = 0xFFFFFF;
constexpr std::uint32_t kMuted = 0x9A9A9A;      /* labels, empty values, section headings */
constexpr std::uint32_t kDim = 0x555555;        /* an empty table slot */
constexpr std::uint32_t kBar = 0x2B2B2B;        /* the selected row, the active tab */
constexpr std::uint32_t kPanel = 0x1C1C1C;      /* chips, buttons, the tab strip */
constexpr std::uint32_t kPressed = 0x3A3A3A;
constexpr std::uint32_t kRing = 0x303030;       /* range rings */
constexpr std::uint32_t kRingFov = 0x4A4A4A;    /* the rings inside the field of view */
constexpr std::uint32_t kWedge = 0x5AAAFF;      /* the field-of-view wedge, drawn faintly */
constexpr std::uint32_t kAxis = 0x555555;
constexpr std::uint32_t kOk = 0x66CC88;         /* link found */
constexpr std::uint32_t kWarn = 0xF0B400;       /* searching, paused */
constexpr std::uint32_t kBad = 0xFF6666;        /* lost, error */
constexpr std::uint32_t kAccent = 0x5AAAFF;     /* the active tab's mark */
} // namespace pal
} // namespace mmwui
