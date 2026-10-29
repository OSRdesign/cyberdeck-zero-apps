/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the deductive solver (no LVGL; tests/test_solver.cpp). It works on what the player sees (board.hpp
 * View) and only ever answers with what is PROVEN: a cell is reported safe or a mine only when every layout that
 * fits the numbers says so. It never guesses. Flags on the board are NOT trusted (the player may have flagged wrongly):
 * a flagged cell counts as an unknown cell, so a hint stays right whatever flags are down.
 *
 *   Rules, in this order, repeated until nothing new is found:
 *     1. single number: remaining mines == 0 -> every unknown neighbour is safe; remaining == unknown neighbours ->
 *        all are mines (mines already deduced are subtracted).
 *     2. subset: if the unknown neighbours of number A are all unknown neighbours of number B, the cells of B not in
 *        A hold B.remaining - A.remaining mines: none -> safe, all of them -> mines.
 *     3. whole board count: no mine left to find -> every unknown cell is safe; as many left as unknown cells -> all
 *        are mines.
 *     4. enumeration: the frontier (unknown cells next to a number) is split into connected groups; a group of at
 *        most kEnumLimit cells has every layout that fits its numbers enumerated (a node budget bounds the work); a
 *        cell that is a mine in all of them is a mine, in none of them safe. A larger group, or one that exceeds
 *        the budget, is left undecided ("cannot decide").
 *   Not done: reasoning that combines groups through the global mine count beyond rule 3. A board that needs it is
 *   not "solvable without guessing" for this solver (the fair generator only keeps boards the solver completes by following its own hints).
 *
 *   Budget   a work counter shared by a sequence of calls (one unit is one number looked at, one pair of numbers
 *            compared, one enumeration node). With a limit, the solver stops when it is spent and says so
 *            (Deduction::exhausted); what it had proven so far is still right.
 */
#pragma once

#include "board.hpp"

#include <vector>

namespace minefield {

constexpr int kEnumLimit = 20;            /* largest frontier group that is enumerated */
constexpr long kEnumNodeCap = 60000;      /* enumeration nodes per group */

struct Budget {
    long limit = -1;                      /* < 0: unlimited */
    long used = 0;
    int enum_limit = kEnumLimit;          /* largest frontier group enumerated (0 = rule 4 off; the tests use it) */
    bool spend(long n = 1)
    {
        used += n;
        return limit < 0 || used <= limit;
    }
    bool exhausted() const { return limit >= 0 && used > limit; }
};

struct Deduction {
    std::vector<int> safe;                /* hidden (or wrongly flagged) cells proven safe */
    std::vector<int> mines;               /* cells proven to be mines (flagged ones included) */
    bool exhausted = false;               /* the budget ran out */
};

/* Everything provable from the view. With stop_at_first_safe the search ends as soon as one unflagged safe cell is
 * known (Deduction::safe then holds what was found up to then). */
void deduce(const View &view, Deduction &out, Budget *budget = nullptr, bool stop_at_first_safe = false);

/* A hidden cell that is provably safe (preferring cells without a flag), or -1: "none provable". */
int find_safe_cell(const View &view, Budget *budget = nullptr);
/* A cell that is provably a mine and not flagged yet, or -1. */
int find_mine(const View &view, Budget *budget = nullptr);

/* Plays the board from `first_click` using only the solver, one cell at a time, each the cell find_safe_cell() names (so
 * it is exactly the game of a player who follows the hints): true when every safe cell gets opened. `board` must hold a mine layout (Board::place); it is not changed. With a
 * budget, false also when the budget ends (Budget::exhausted() tells). `rounds` (optional) gets the number of rounds. */
bool is_solvable_without_guessing(const Board &board, int first_click, Budget *budget = nullptr, int *rounds = nullptr);

} // namespace minefield
