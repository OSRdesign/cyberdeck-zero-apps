/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike UI: the card art, drawn from scratch by a small anti-aliased rasteriser (pure, no LVGL; tests/test_layout.cpp
 * checks the shapes). Own design: nothing here is taken from any other card set.
 *
 *   render_suit(suit, size)         the suit symbol as an 8-bit coverage mask, size x size (heart, spade, diamond, club
 *                                   as vector shapes, each filling its box so that the same size reads the same)
 *   render_emblem(kind, w, h)       the face-card emblem: Jack a sword, Queen a tiara, King a crown
 *   render_back(design, w, h)       a card back as ARGB8888 (0xAARRGGBB, rounded corners in the alpha): design 0 a blue
 *                                   diamond lattice, design 1 a red rosette
 *   pip_layout(rank)                where the pips of a number card go, in 0..1 of the pip area (x, y, flipped)
 *   suit_rgb / felt_rgb / back_rgb  the palette (two-colour deck or four-colour deck)
 * The UI makes each mask once per card size and draws it with the suit's colour (an A8 image recoloured).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace klondike {
namespace art {

struct Mask {
    int w = 0, h = 0;
    std::vector<std::uint8_t> a; /* w * h coverage, row by row */
    std::uint8_t at(int x, int y) const { return a[static_cast<size_t>(y) * w + x]; }
};

enum Emblem { Jack = 0, Queen = 1, King = 2 };

Mask render_suit(int suit, int size);
Mask render_emblem(int kind, int w, int h);
std::vector<std::uint32_t> render_back(int design, int w, int h);

struct Pip {
    float x, y;    /* centre, 0..1 of the pip area */
    bool flipped;  /* drawn upside down (the lower half of a number card) - only the big suit is symmetrical, kept for the API */
};
/* The pips of ranks 2..10 (rank 1 is one big symbol in the middle, 11..13 an emblem). Returns the count (0 for others). */
int pip_layout(int rank, Pip out[10]);

constexpr int kFelts = 3;
constexpr int kBacks = 2;
std::uint32_t suit_rgb(int suit, bool four_colour);
std::uint32_t felt_rgb(int felt);
const char *felt_name(int felt);
const char *back_name(int design);
constexpr std::uint32_t kCardFaceRgb = 0xFBF9F3;
constexpr std::uint32_t kCardEdgeRgb = 0x8E8E8E;

} // namespace art
} // namespace klondike
