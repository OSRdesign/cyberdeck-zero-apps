/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the board (no LVGL, unit tested).
 *
 * A tile is a set of cable ends (a 4-bit mask: N 1, E 2, S 4, W 8). Its TYPE never changes: end (one cable, a
 * computer), straight, corner, tee, cross (only the server can be one besides a tee). Playing only rotates tiles.
 *
 * generate(): a random spanning tree grown from the server (growing-tree, newest-or-random cell, with a bias against
 * long straight runs; the server has 3 or 4 cables, every other tile at most 3), the tree's tiles are the solved
 * orientations (kept per tile for a hint), then every tile is turned by 0..3 quarter turns (scramble, rotation only)
 * until the board is not already a win.
 *
 * The win is a property of the STATE, not a comparison with the stored layout: every tile is powered from the server
 * (so every computer is connected and every tile is used) and no cable end points at nothing or at a tile that does not
 * answer it. Loops are allowed. Wrap mode: the edges meet (a cable off the right edge enters the left one).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace netwalk {

enum Dir : std::uint8_t { kN = 1, kE = 2, kS = 4, kW = 8 };

enum class Kind : std::uint8_t { None, End, Straight, Corner, Tee, Cross };

constexpr std::uint8_t rot_cw(std::uint8_t m)
{
    return static_cast<std::uint8_t>(((m << 1) | (m >> 3)) & 15);
}
constexpr std::uint8_t rot_ccw(std::uint8_t m)
{
    return static_cast<std::uint8_t>(((m >> 1) | (m << 3)) & 15);
}
constexpr std::uint8_t opposite(std::uint8_t d)
{
    return rot_cw(rot_cw(d));
}
int popcount4(std::uint8_t m);
Kind kind_of(std::uint8_t mask);

class Board {
public:
    Board() = default;

    /* a new board; the same arguments always give the same board */
    static Board generate(int w, int h, bool wrap, std::uint64_t seed);
    /* a hand-made board: `masks` is the layout, which also becomes the solved orientation; the tiles start as given */
    static Board from_masks(int w, int h, bool wrap, int server, const std::vector<std::uint8_t> &masks);

    int w() const { return w_; }
    int h() const { return h_; }
    int size() const { return w_ * h_; }
    bool wrap() const { return wrap_; }
    int server() const { return server_; }
    bool valid() const { return w_ > 0 && h_ > 0 && static_cast<int>(mask_.size()) == w_ * h_; }

    std::uint8_t mask(int i) const { return mask_[static_cast<std::size_t>(i)]; }
    std::uint8_t solved_mask(int i) const { return solved_[static_cast<std::size_t>(i)]; }
    /* quarter turns clockwise the scramble applied to the tile */
    int scramble(int i) const { return scramble_[static_cast<std::size_t>(i)]; }
    Kind kind(int i) const { return kind_of(solved_[static_cast<std::size_t>(i)]); }
    bool locked(int i) const { return locked_[static_cast<std::size_t>(i)] != 0; }
    void set_locked(int i, bool on) { locked_[static_cast<std::size_t>(i)] = on ? 1 : 0; }

    /* the tile next to i in direction d (a Dir), or -1 (off the edge when not wrapping) */
    int neighbor(int i, std::uint8_t d) const;

    /* one quarter turn, whatever the lock (the game applies the lock); false when it changes nothing (a cross) */
    bool rotate(int i, bool clockwise);
    /* put a tile in any state (tests: a layout other than the stored solution) */
    void set_mask(int i, std::uint8_t m) { mask_[static_cast<std::size_t>(i)] = m & 15; }
    /* set the tile to the solved orientation (tests, a hint) */
    void solve_tile(int i) { mask_[static_cast<std::size_t>(i)] = solved_[static_cast<std::size_t>(i)]; }

    /* 1 per tile the server's power reaches along answered cable ends (the server itself included) */
    std::vector<std::uint8_t> powered() const;
    int powered_count() const;
    /* a cable end of tile i that points at nothing or at a tile with no end facing back */
    bool has_loose_end(int i) const;
    /* every tile powered and no loose end anywhere */
    bool is_won() const;

private:
    int w_ = 0, h_ = 0;
    bool wrap_ = false;
    int server_ = 0;
    std::vector<std::uint8_t> mask_, solved_, scramble_, locked_;
};

} // namespace netwalk
