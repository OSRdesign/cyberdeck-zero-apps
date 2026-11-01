/*
 * SPDX-License-Identifier: MIT
 *
 * Shared helpers of the unit tests (test_core.cpp has main(); the other files add their groups).
 * Every .cpp file of tests/ is linked into one binary, test-net-walk, by ../../_common/build_app.sh host.
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

/* a failure that names the case (seed, size) it happened on */
#define CHECK_AT(c, seed, w, wrap)                                                                                     \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            if (g_fail < 40)                                                                                           \
                std::fprintf(stderr, "%s:%d: CHECK(%s) failed, seed %llu, %dx%d%s\n", __FILE__, __LINE__, #c,          \
                             static_cast<unsigned long long>(seed), w, w, (wrap) ? " wrap" : "");                     \
        }                                                                                                              \
    } while (0)

/* A board from rows of text, one row per line of tiles, tiles separated by spaces: a tile is the letters of the cable
 * ends it has (N, E, S, W in any order), or "-" for none. The server is the tile at (server_x, server_y). */
netwalk::Board board_from_text(const std::vector<std::string> &rows, bool wrap, int server_x, int server_y);

void test_board_group();
void test_game_group();
void test_scores_group();
void test_layout_group();
void test_font_group();
void test_package_group();
