/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the colours of the owner-approved boards (option A, cream portable), named once.
 */
#pragma once

#include <cstdint>

namespace cassette {
namespace ui {
namespace col {

/* body and panels */
constexpr std::uint32_t kScreen = 0x000000;
constexpr std::uint32_t kBody = 0xD8D1BC;       /* the cream deck */
constexpr std::uint32_t kBodyEdge = 0xB9B29C;
constexpr std::uint32_t kHinge = 0x8E8772;      /* hinge pins, key lips, slider track, scroll thumb */
constexpr std::uint32_t kFrame = 0x9B947F;      /* the 3 px frame of the window and the dark panels */
constexpr std::uint32_t kWindow = 0x1B1C1E;     /* cassette window, waveform window */
constexpr std::uint32_t kPanel = 0x232426;      /* meter panel, stepper frames */
constexpr std::uint32_t kStrip = 0xBDB59F;      /* info strips, selected row */
constexpr std::uint32_t kInk = 0x1F1F1F;
constexpr std::uint32_t kInkSoft = 0x33302A;
constexpr std::uint32_t kInkMuted = 0x4A4636;
constexpr std::uint32_t kRow = 0xE6DFCA;
constexpr std::uint32_t kScrollTrack = 0xC9C1AB;

/* keys and buttons */
constexpr std::uint32_t kKeyTop = 0x3A3B3E, kKeyBottom = 0x232427, kKeyHi = 0x5A5C61, kKeyText = 0xEDEBE4;
constexpr std::uint32_t kRecTop = 0xC8352B, kRecBottom = 0x97241C, kRecHi = 0xE0675D;
constexpr std::uint32_t kOnTop = 0x7A1B15, kOnBottom = 0x9A241C, kOnInset = 0x3D0D0A;
constexpr std::uint32_t kOkTop = 0x2E8B4F, kOkBottom = 0x1F6A3A, kOkHi = 0x5BBF7F;
constexpr std::uint32_t kReplaceBg = 0x2A1412, kReplaceText = 0xFFB4A8;
constexpr std::uint32_t kWhite = 0xFFFFFF;

/* meter */
constexpr std::uint32_t kLedGreen = 0x3FD166, kLedAmber = 0xF0B400, kLedRed = 0xFF4D3A;
constexpr std::uint32_t kInLabel = 0xD8DCDF, kScale = 0xC9CDD0, kAmber = 0xF0B400;
constexpr std::uint32_t kPeakOff = 0x4A1612, kPeakOn = 0xE5372B, kPeakEdge = 0x14171A;
constexpr std::uint32_t kMonOff = 0x163F22, kMonOn = kLedGreen; /* the MON lamp: dark green, the low bars' green */
/* the meter panel's list (portrait, square): MON, LIM and LOW CUT off are a neutral dark grey dot (no glow) with the label at
 * 55 % opacity; on, the lamp green with its glow and the label at full opacity */
constexpr std::uint32_t kListLampOff = 0x3A3F3F;
constexpr unsigned kListLabelOffOpa = 140;
constexpr std::uint32_t kCounterBox = 0x0A0A0A, kCounterEdge = 0x3A3F44, kCell = 0x141516, kDigit = 0xF4F1E8;

/* cassette */
constexpr std::uint32_t kShell = 0x2E2F33, kShellEdge = 0x555963, kPaper = 0xF2EBD3, kStripe = 0xC23B2A;
constexpr std::uint32_t kPaperLine = 0xB9B29C, kLabelInk = 0x22252B, kLabelGrey = 0x555555;
constexpr std::uint32_t kHubWindow = 0x0E0F10, kHub = 0xE9E7E0, kHubInner = 0x2A2B2E, kHead = 0x1A1B1E;

/* lamps */
constexpr std::uint32_t kLampGreen = 0x3FD166, kLampRed = 0xE5372B, kRecWord = 0x8E1D15, kPlayWord = 0x1F6A3A;

/* countdown overlay */
constexpr std::uint32_t kShade = 0x0A0A0A;      /* at 84 % */
constexpr std::uint32_t kRingBack = 0x3A3B3E, kRingRed = 0xE5372B, kHint = 0x9B9FA3;

/* waveform */
constexpr std::uint32_t kWaveCut = 0x6B6F73, kChip = 0xEDEBE4, kChipText = 0x232426;
constexpr std::uint32_t kFlag = 0x3FD166, kFlagText = 0x0E3A1B, kMarker = 0xE5372B, kRuler = 0x9B9FA3;

} // namespace col
} // namespace ui
} // namespace cassette
