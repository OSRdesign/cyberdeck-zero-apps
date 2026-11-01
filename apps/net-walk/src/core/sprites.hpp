/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the pixel art of the two devices (pure data, no LVGL, unit tested). A sprite is rows of characters, one
 * per logical pixel; the UI paints each run of one character as a rectangle, so every edge stays crisp at any scale.
 *
 *   Monitor  a beige desktop CRT drawn upright (only its cable turns): bevelled bezel, inset screen blue when it has no
 *            power and lime with two text lines when it has, a power LED, a neck and a base.
 *   Server   a dark rack tower: status LEDs (green, one amber), a red power button (the only red), vent slots, two drive
 *            bays with handles, feet.
 *
 * Two detail levels by the tile's size in logical pixels: small (under kSmallTile: 9 x 8 monitor, 7 x 10 server) and
 * normal (13 x 14 monitor, 11 x 13 server), the normal one repeated 2x, 3x ... on a tile that has room for it.
 */
#pragma once

namespace netwalk {

enum class SpriteKind { Monitor, Server };

struct Sprite {
    int w = 0, h = 0;
    const char *const *rows = nullptr; /* h strings of w characters; '.' is transparent */
};

constexpr int kSmallTile = 19; /* tiles under this many logical pixels get the small devices */

bool use_small_sprite(int tile);
const Sprite &sprite(SpriteKind kind, bool small);
/* whole-number magnification of a sprite on a tile: at least 1, and the sprite leaves 3 px of cable on every side */
int sprite_factor(const Sprite &s, int tile);
/* the colour (0xRRGGBB) of a character, or -1 when it is transparent; `lit` = the monitor has power */
long sprite_color(SpriteKind kind, char c, bool lit);

} // namespace netwalk
