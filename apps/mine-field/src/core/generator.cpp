/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: laying the mines (see generator.hpp).
 */
#include "generator.hpp"

#include "rng.hpp"
#include "solver.hpp"

#include <algorithm>
#include <cstdlib>

namespace minefield {

std::vector<std::uint8_t> place_mines(int w, int h, int mines, std::uint64_t seed, int first_click)
{
    const int n = w * h;
    std::vector<std::uint8_t> map(static_cast<std::size_t>(n), 0);
    if (w <= 0 || h <= 0) return map;
    const int fx = first_click >= 0 && first_click < n ? first_click % w : -10;
    const int fy = first_click >= 0 && first_click < n ? first_click / w : -10;
    std::vector<int> cand;
    cand.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int x = i % w, y = i / w;
        if (std::abs(x - fx) <= 1 && std::abs(y - fy) <= 1) continue;   /* the first cell and its neighbours */
        cand.push_back(i);
    }
    const int count = std::min(mines, static_cast<int>(cand.size()));
    Rng rng(seed);
    for (int k = 0; k < count; ++k) {
        const int j = k + static_cast<int>(rng.below(static_cast<std::uint32_t>(cand.size() - static_cast<std::size_t>(k))));
        std::swap(cand[static_cast<std::size_t>(k)], cand[static_cast<std::size_t>(j)]);
        map[static_cast<std::size_t>(cand[static_cast<std::size_t>(k)])] = 1;
    }
    return map;
}

GenResult generate(int w, int h, int mines, std::uint64_t seed, int first_click, bool fair, const GenOptions &opt)
{
    GenResult r;
    r.fair_requested = fair;
    r.seed_used = seed;
    r.mines = place_mines(w, h, mines, seed, first_click);
    if (!fair) return r;

    Budget budget;
    budget.limit = opt.work_budget;
    Board board(w, h, mines);
    for (int k = 0;; ++k) {
        if (k > 0) {
            r.seed_used = derive_seed(seed, k);
            r.mines = place_mines(w, h, mines, r.seed_used, first_click);
            r.attempts = k + 1;
        }
        board.place(r.mines);
        const bool ok = is_solvable_without_guessing(board, first_click, &budget);
        r.solver_work = budget.used;
        if (ok) return r;
        if (k + 1 >= opt.max_attempts || budget.exhausted()) break;
    }
    r.fair_failed = true;
    return r;
}

} // namespace minefield
