/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the colours of the P1 canvas (canvas-vhs-p1/gen_vhs.py), named once.
 */
#pragma once

#include <cstdint>

namespace vhs {
namespace ui {
namespace col {

constexpr std::uint32_t kPanel = 0x1B1E1E;     /* the dark panel under the header */
constexpr std::uint32_t kButton = 0x2A2A2A;    /* .btn */
constexpr std::uint32_t kButtonPressed = 0x444444;
constexpr std::uint32_t kText = 0xDDDDDD;      /* button labels, the mode word, the NoCamera title */
constexpr std::uint32_t kDanger = 0xA62E17;    /* the Square class's Delete row on the cream sheet (canvas) */
constexpr std::uint32_t kStatus = 0xA8A8A8;    /* the Square class's status line, hints and counts (canvas) */
constexpr std::uint32_t kSheetKeyDark = 0x4A4535; /* the Square class's sheet keys and detail (contrast, owner's canvas) */
constexpr std::uint32_t kMuted = 0x9A9A9A;     /* the note, the NoCamera text, a button's second line */
constexpr std::uint32_t kPrimary = 0x8A5A2B;   /* the primary button (Cancel of the countdown, M2) */
constexpr std::uint32_t kShutterBg = 0x141414; /* the shutter's face */
constexpr std::uint32_t kShutterRing = 0xDDDDDD;
constexpr std::uint32_t kWhite = 0xFFFFFF;
constexpr std::uint32_t kCream = 0xDCD4BD;     /* the toast */
constexpr std::uint32_t kInk = 0x222222;       /* the toast's text */
constexpr std::uint32_t kHot = 0xE0553A;       /* the red toast, REC (M3) */
constexpr std::uint32_t kLamp = 0x3FBF6A;      /* the Sound-on lamp's dot (canvas Recording-480x320) */
constexpr std::uint32_t kAccent = 0xF0B400;    /* "locked" (canvas Scenes-480x320) */
constexpr std::uint32_t kThumb = 0x1A2328;     /* a row's thumbnail box before (or without) its picture */
constexpr std::uint32_t kSheetKey = 0x6B6552;  /* the sheet's header, keys and note (canvas SceneOptions-480x320) */
constexpr std::uint32_t kSheetSel = 0xC9C0A5;  /* the sheet's highlighted row */
constexpr std::uint32_t kPicture = 0x000000;   /* the picture box's border (letterbox / pillarbox) */

} // namespace col
} // namespace ui
} // namespace vhs
