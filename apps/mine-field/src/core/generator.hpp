/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: laying the mines (no LVGL; tests/test_solver.cpp). Mines are laid on the FIRST reveal, never on the
 * first dug cell or its neighbours (so the first tap always opens an area). The layout is a pure function of
 * (w, h, mines, seed, first_click, fair): the same seed and the same first cell give the same board.
 *
 *   place_mines   the classic layout: the allowed cells are shuffled (partial Fisher-Yates from the seed) and the
 *                 first `mines` of them get a mine. Instant.
 *   generate      fair = false: place_mines(seed). fair = true: tries place_mines(derive_seed(seed, k)) for
 *                 k = 0, 1, ... until the solver (solver.hpp) opens the whole board from the first click without a
 *                 guess; k = 0 is the plain board. At most opt.max_attempts boards and opt.work_budget solver
 *                 units are spent (a unit is about 0.05 microseconds on a desktop core); when the search runs out the
 *                 LAST board tried is returned with fair_failed = true (the board is still playable, only not
 *                 proven guess-free). `attempts` is how many boards were laid, `solver_work` the units used.
 */
#pragma once

#include "board.hpp"

#include <cstdint>
#include <vector>

namespace minefield {

constexpr int kFairMaxAttempts = 200;
constexpr long kFairWorkBudget = 4000000;

struct GenOptions {
    int max_attempts = kFairMaxAttempts;
    long work_budget = kFairWorkBudget;
};

struct GenResult {
    std::vector<std::uint8_t> mines;       /* one byte per cell */
    std::uint64_t seed_used = 0;           /* the seed of the board returned (derived when fair) */
    int attempts = 1;
    bool fair_requested = false;
    bool fair_failed = false;              /* fair was asked for and no proven guess-free board was found */
    long solver_work = 0;
};

std::vector<std::uint8_t> place_mines(int w, int h, int mines, std::uint64_t seed, int first_click);

GenResult generate(int w, int h, int mines, std::uint64_t seed, int first_click, bool fair,
                   const GenOptions &opt = GenOptions());

} // namespace minefield
