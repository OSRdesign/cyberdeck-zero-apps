/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the solver and the winnable deals (no LVGL; tests/test_solver.cpp, measured by
 * tests/bench/solver_bench.cpp).
 *
 *   Solver::solve(board, rules)   decides whether the position can be won, looking at every card (face-down ones
 *                                 too: it answers "can this deal be won by anyone", not "by a player who cannot see
 *                                 the cards"). Verdict:
 *       Winnable      a win was found; `solution` is the full list of moves (draws and recycles included) from the
 *                     position, replayed through the rules before it is returned, so the proof is never wrong.
 *       NotWinnable   the whole search finished without a win: no sequence of moves wins. (The search remembers
 *                     positions by a 64-bit hash; a hash collision could in theory cut a branch, at odds far below
 *                     one in a million per call.)
 *       Unknown       the node budget (or the depth limit) ran out first.
 *   Deterministic: no clock, no randomness; the same position, rules and limits give the same answer, the same
 *   solution and the same node count on every board. Not thread safe per object; one Solver per thread.
 *
 *   How: depth-first search with a transposition table (open addressing, 2 x max_nodes slots of 8 bytes, allocated
 *   once per Solver and reused), on these reductions, each of which keeps every win:
 *     - safe moves to the foundations (board.hpp safe_to_foundation) are played at once from the tableau; from the
 *       waste with draw 1; and from anywhere in the stock with draw 1 and unlimited passes (drawing is free there);
 *     - the stock is not searched card by card: each card that drawing (and at most one recycle) can bring to the
 *       waste top is one move "draw until it shows, then play it"; draws commute with every other move, so nothing
 *       is lost;
 *     - positions are compared up to the order of the columns, foundations by suit, and with draw 1 and unlimited
 *       passes without the stock pointer (every rotation can be reached by drawing); a position seen again with no
 *       fewer recycles used is not searched again (fewer recycles is never worse);
 *     - a whole column moved to another empty column, and a foundation card taken back while it is safe (nothing
 *       could ever go on it), are not tried.
 *   Moves are tried in this order: to a foundation, tableau moves that turn a card over (the column with most
 *   face-down cards first), cards from the stock (fewest draws first), other tableau moves, foundation to tableau.
 *   Two passes within one budget: the first leaves out the tableau moves that change nothing now (a run moved
 *   between two cards of the same rank and colour, a whole column moved off when no card could use the empty
 *   column) and finds most wins several times faster; only when it ran out of moves after leaving some out does the
 *   complete second pass go on with the rest of the budget, so NotWinnable is always a proof of the full rules.
 *
 *   find_winnable_deal(user, rules)   the deal for "winnable deal number `user`": tries the deal numbers
 *       user, mix(user, 1), mix(user, 2), ... (mix below) with the solver at `nodes_per_try` each and returns the
 *       first one proven Winnable, with the solver's solution. The mapping depends only on the user number, the rules
 *       that change winnability (draw, passes, foundation_back, fill) and the limits: keep the defaults so that a
 *       number gives the same deal on every board and in every version. mix(u, k) = the SplitMix64 output of
 *       state u * 2^32 + k, truncated to 32 bits.
 */
#pragma once

#include "board.hpp"
#include "rules.hpp"

#include <cstdint>
#include <vector>

namespace klondike {

struct SolverLimits {
    std::uint32_t max_nodes = 100000;  /* positions expanded; about 1 s on a Pi 3A+ (README, measurements) */
    std::uint16_t max_depth = 400;     /* search depth (each stock play is one level) */
};

enum class Verdict { Winnable, NotWinnable, Unknown };
const char *verdict_name(Verdict v);

struct SolveResult {
    Verdict verdict = Verdict::Unknown;
    std::uint32_t nodes = 0;
    std::vector<Move> solution;        /* Winnable only */
};

class Solver {
public:
    explicit Solver(const SolverLimits &limits = SolverLimits());
    ~Solver();
    Solver(const Solver &) = delete;
    Solver &operator=(const Solver &) = delete;

    SolveResult solve(const Board &start, const Rules &rules);

private:
    struct Impl;
    Impl *impl_;
};

struct WinnableLimits {
    std::uint32_t nodes_per_try = 30000;
    std::uint32_t max_tries = 256;
};

struct WinnableDeal {
    bool found = false;
    std::uint32_t deal = 0;            /* the deal number to play (= user number when that one is proven) */
    std::uint32_t tries = 0;
    std::uint64_t nodes = 0;           /* all tries together */
    std::vector<Move> solution;
};

std::uint32_t winnable_candidate(std::uint32_t user, std::uint32_t k);
WinnableDeal find_winnable_deal(std::uint32_t user, const Rules &rules, const WinnableLimits &limits = WinnableLimits());

} // namespace klondike
