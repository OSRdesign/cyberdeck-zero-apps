/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk: the Palm OS 4 colour palette and the drawing primitives. Everything is drawn from LOGICAL pixels: a logical
 * pixel is `scale` x `scale` screen pixels, so edges stay crisp at every size. Only flat rectangles are drawn (no
 * antialiasing, no images, no font files): tiles, bevels, buttons and the bold bitmap text are all rectangles.
 */
#pragma once

#include "layout.hpp"
#include "pixfont.hpp"

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace nwui {

/* Palm OS 4 (256 colour, 2001) look */
namespace pal {
constexpr std::uint32_t kLetterbox = 0x808080; /* the Palm desktop grey round the canvas */
constexpr std::uint32_t kNavy = 0x000080;      /* window title bars (Size, Scores, Network up!), the key cursor */
constexpr std::uint32_t kWhite = 0xFFFFFF;
constexpr std::uint32_t kBlack = 0x000000;
constexpr std::uint32_t kFace = 0xCCCCCC;      /* window face */
constexpr std::uint32_t kHi = 0xFFFFFF;        /* bevel light */
constexpr std::uint32_t kLo = 0x666666;        /* bevel shade */
constexpr std::uint32_t kMuted = 0x666666;     /* a read-only value (the row affordance rule: info rows draw grey) */
constexpr std::uint32_t kBoard = 0xC0D8F0;     /* board ground, a pale blue-grey */
constexpr std::uint32_t kTileHi = 0xE4EEF9;
constexpr std::uint32_t kTileLo = 0x8EA8C8;
constexpr std::uint32_t kTileLocked = 0xA9BDD6;
constexpr std::uint32_t kCable = 0x404040;     /* idle cable */
constexpr std::uint32_t kCableEdge = 0x1C1C1C;
constexpr std::uint32_t kPower = 0x00CC00;     /* energised cable */
constexpr std::uint32_t kPowerEdge = 0x006600;
constexpr std::uint32_t kFlash = 0xFFFF99;     /* the win flash */
/* the devices (monitor, server) have their own colours in core/sprites.cpp */
constexpr std::uint32_t kLockBody = 0xFFCC00;
constexpr std::uint32_t kLockEdge = 0x664400;
} // namespace pal

struct Painter {
    lv_layer_t *layer = nullptr;
    int scale = 1;
    int ox = 0, oy = 0; /* screen position of the logical canvas's corner */

    static lv_color_t color(std::uint32_t rgb) { return lv_color_hex(rgb); }

    /* a filled rectangle in logical pixels */
    void rect(int x, int y, int w, int h, std::uint32_t rgb) const
    {
        if (w <= 0 || h <= 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = color(rgb);
        d.bg_opa = LV_OPA_COVER;
        d.radius = 0;
        d.border_width = 0;
        const lv_area_t a{ox + x * scale, oy + y * scale, ox + (x + w) * scale - 1, oy + (y + h) * scale - 1};
        lv_draw_rect(layer, &d, &a);
    }
    void rect(const netwalk::Rect &r, std::uint32_t rgb) const { rect(r.x, r.y, r.w, r.h, rgb); }
    /* a 1 logical pixel outline */
    void outline(int x, int y, int w, int h, std::uint32_t rgb) const
    {
        rect(x, y, w, 1, rgb);
        rect(x, y + h - 1, w, 1, rgb);
        rect(x, y + 1, 1, h - 2, rgb);
        rect(x + w - 1, y + 1, 1, h - 2, rgb);
    }
    /* a bevelled box: `raised` light top left and shade bottom right, else the reverse */
    void bevel(int x, int y, int w, int h, std::uint32_t face, bool raised) const
    {
        rect(x, y, w, h, face);
        const std::uint32_t tl = raised ? pal::kHi : pal::kLo, br = raised ? pal::kLo : pal::kHi;
        rect(x, y, w, 1, tl);
        rect(x, y, 1, h, tl);
        rect(x, y + h - 1, w, 1, br);
        rect(x + w - 1, y, 1, h, br);
    }

    /* bold text, top left at (x, y) */
    void text(const std::string &s, int x, int y, std::uint32_t rgb) const
    {
        int cx = x;
        for (char c : s) {
            for (int r = 0; r < netwalk::kGlyphRows; ++r) {
                const unsigned row = netwalk::glyph_row(c, r);
                if (!row) continue;
                const unsigned m = (row << 1) | row; /* the bold smear: 6 columns, bit 5 = leftmost */
                int c0 = 0;
                while (c0 < 6) {
                    if (!((m >> (5 - c0)) & 1u)) {
                        ++c0;
                        continue;
                    }
                    int c1 = c0;
                    while (c1 + 1 < 6 && ((m >> (5 - (c1 + 1))) & 1u)) ++c1;
                    rect(cx + c0, y + r, c1 - c0 + 1, 1, rgb);
                    c0 = c1 + 1;
                }
            }
            cx += netwalk::kAdvance;
        }
    }
    void text_right(const std::string &s, int right, int y, std::uint32_t rgb) const
    {
        text(s, right - netwalk::text_width(s), y, rgb);
    }
    void text_center(const std::string &s, int cx, int y, std::uint32_t rgb) const
    {
        text(s, cx - netwalk::text_width(s) / 2, y, rgb);
    }
};

} // namespace nwui
