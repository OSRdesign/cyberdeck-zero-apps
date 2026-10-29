/*
 * SPDX-License-Identifier: MIT
 *
 * Shared helpers of the unit tests (test_core.cpp has main(); the other files add their groups).
 * Every .cpp file of tests/ is linked into one binary, test-mine-field, by ../../_common/build_app.sh host.
 */
#pragma once

#include "board.hpp"
#include "game.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

/* A small deterministic generator for the random playouts (xorshift64*), independent of the core's. */
struct TestRng {
    std::uint64_t s;
    explicit TestRng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

/* A game on a hand-made board. `mines` rows use '*' for a mine and '.' for none (all rows the same width, at least
 * 5 x 5); `states` rows (optional, same shape) use 'h' hidden, 'r' revealed, 'f' flagged. The cell (fx, fy) is the first
 * click: revealed, and no mine in it or next to it. The game is Playing. CHECKs that it loads. */
minefield::Game make_game(const std::vector<std::string> &mines, int fx, int fy,
                          const std::vector<std::string> &states = {}, std::uint32_t clock_ms = 0);

/* Plays one game from a seed the way a test wants: all safe cells opened by the true layout in index order. */
void test_solver_group();
void test_store_group();
void test_view_group();
