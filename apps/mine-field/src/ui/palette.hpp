/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: every colour the screen uses, in one place. Where the launcher's toolkit has a token (cp0_ui_list.h:
 * text white, muted grey, the accent blue, warn amber, bad red, the bar grey) the name here is that token; only the board
 * art has colours of its own. Digits use the Okabe-Ito set (lightened for a dark ground), whose hues stay apart for the
 * common kinds of colour blindness, and every digit is also drawn as its own shape: colour is never the only signal.
 */
#pragma once

#include "cp0_ui_list.h"

#include <cstdint>

namespace mfui::pal {

/* from the toolkit */
constexpr std::uint32_t kText = CP0_UI_LIST_TEXT_RGB;
constexpr std::uint32_t kMuted = CP0_UI_LIST_MUTED_RGB;
constexpr std::uint32_t kAccent = CP0_UI_LIST_ACCENT_RGB;
constexpr std::uint32_t kWarn = CP0_UI_LIST_WARN_RGB;
constexpr std::uint32_t kBad = CP0_UI_LIST_BAD_RGB;
constexpr std::uint32_t kBar = CP0_UI_LIST_BAR_RGB;                 /* a button's face (the list's highlight bar) */
constexpr std::uint32_t kBarPressed = CP0_UI_LIST_SCROLLBAR_RGB;    /* a pressed button */
constexpr std::uint32_t kBarBorder = CP0_UI_LIST_MUTED_RGB;
constexpr std::uint32_t kOnAccent = 0x000000;                       /* text on an accent-filled button */

/* the board's own */
constexpr std::uint32_t kGround = 0x101214;       /* behind the cells */
constexpr std::uint32_t kTileHi = 0x8A93A0;       /* a hidden tile's lit top and left edges */
constexpr std::uint32_t kTile = 0x59616D;         /* its face */
constexpr std::uint32_t kTileLo = 0x363B44;       /* its shaded bottom and right edges */
constexpr std::uint32_t kOpen = 0x1B1F25;         /* an opened cell */
constexpr std::uint32_t kOpenLine = 0x2C323A;     /* its hairline */
constexpr std::uint32_t kExploded = 0xB3261E;     /* the cell whose mine was dug */
constexpr std::uint32_t kPole = 0xE9E9E9;         /* a flag's pole and base */
constexpr std::uint32_t kFlag = 0xFF5A1F;         /* a flag's cloth: red-orange */
constexpr std::uint32_t kMineBody = 0xF2F2F2;     /* a mine */
constexpr std::uint32_t kMineGlint = 0x59616D;
constexpr std::uint32_t kCross = 0xFFFFFF;        /* the cross over a wrong flag */
constexpr std::uint32_t kCursor = kAccent;        /* the keyboard cursor ring */
constexpr std::uint32_t kHint = kWarn;            /* the hint ring */
constexpr std::uint32_t kFlash = kWarn;           /* the neighbours that flash on a chord that does not match */
constexpr std::uint32_t kSweep = 0xFFFFFF;        /* the win sweep's band (translucent) */

/* digits 1..8 */
constexpr std::uint32_t kDigit[9] = {0, 0x56B4E9, 0xF0E442, 0xE69F00, 0xCC79A7, 0x2EC4A0, 0xB8A1FF, 0xE8E8E8, 0x9AA7B8};

} // namespace mfui::pal
