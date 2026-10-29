/*
 * SPDX-License-Identifier: MIT
 *
 * Shared helpers of the unit tests (test_core.cpp has main(); test_solver.cpp and test_store.cpp add their groups).
 * Every .cpp file of tests/ is linked into one binary, test-klondike-solitaire, by ../../_common/build_app.sh host.
 */
#pragma once

#include "board.hpp"
#include "game.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

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

/* A small deterministic generator for the random playouts (xorshift64*). */
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

inline klondike::Move mv(int from, int to, int count)
{
    klondike::Move m;
    m.from = static_cast<std::uint8_t>(from);
    m.to = static_cast<std::uint8_t>(to);
    m.count = static_cast<std::uint8_t>(count);
    return m;
}

/* T(1) .. T(7) and F(1) .. F(4): the piles as the board text counts them. */
inline int T(int n) { return klondike::tableau_pile(n - 1); }
inline int F(int n) { return klondike::foundation_pile(n - 1); }

/* A position from its parts (board.hpp text format): `found` is the 'f' line, `cols` the 7 columns, `waste` and
 * `stock` (bottom first). Every card not named goes to the bottom of the stock (in id order) unless rest_to_stock is
 * false. CHECKs that it parses. */
klondike::Board position(const std::string &found, const std::vector<std::string> &cols, const std::string &waste,
                         const std::string &stock = "", bool rest_to_stock = true, int recycles = 0);

void test_solver_group();
void test_store_group();
void test_layout_group();
void test_session_group();
void test_winsim_group();
