/*
 * SPDX-License-Identifier: MIT
 *
 * Solver measurements (not a unit test: not built by build_app.sh). Build and run in WSL, or on a board:
 *   g++ -std=gnu++17 -O2 -I src/core src/core/*.cpp tests/bench/solver_bench.cpp -o /tmp/solver-bench
 *   /tmp/solver-bench [deals=200] [max_nodes=100000] [winnable nodes per try] [winnable max tries]
 * (from apps/klondike-solitaire). For each rule set: deals 1..N solved from the deal, verdict counts, nodes, time,
 * nodes per second, the slowest solve; then the winnable deal mapping for user numbers 1..N/4.
 */
#include "board.hpp"
#include "solver.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace klondike;

static double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    const int deals = argc > 1 ? std::atoi(argv[1]) : 200;
    const std::uint32_t max_nodes = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 100000;
    WinnableLimits wl;
    if (argc > 3) wl.nodes_per_try = static_cast<std::uint32_t>(std::atoi(argv[3]));
    if (argc > 4) wl.max_tries = static_cast<std::uint32_t>(std::atoi(argv[4]));
    struct Set {
        const char *name;
        int draw, passes;
    } sets[] = {{"draw 1, unlimited", 1, 0}, {"draw 3, unlimited", 3, 0}, {"draw 3, 3 passes", 3, 3},
                {"draw 1, 1 pass", 1, 1}};
    SolverLimits lim;
    lim.max_nodes = max_nodes;
    Solver solver(lim);
    for (const Set &set : sets) {
        Rules r;
        r.draw = set.draw;
        r.passes = set.passes;
        int win = 0, lose = 0, unknown = 0;
        std::uint64_t nodes = 0, win_nodes = 0;
        double slowest = 0, total = 0;
        for (int d = 1; d <= deals; ++d) {
            const double t0 = now_s();
            const SolveResult res = solver.solve(deal_board(static_cast<std::uint32_t>(d)), r);
            const double dt = now_s() - t0;
            total += dt;
            if (dt > slowest) slowest = dt;
            nodes += res.nodes;
            if (res.verdict == Verdict::Winnable) {
                ++win;
                win_nodes += res.nodes;
            } else if (res.verdict == Verdict::NotWinnable) {
                ++lose;
            } else {
                ++unknown;
            }
        }
        std::printf("%-20s deals %d budget %u: winnable %d, not winnable %d, unknown %d | %.0f nodes/s, "
                    "avg %.1f ms, slowest %.1f ms, avg nodes to a win %.0f\n",
                    set.name, deals, max_nodes, win, lose, unknown, nodes / (total > 0 ? total : 1),
                    1000.0 * total / deals, 1000.0 * slowest, win ? double(win_nodes) / win : 0.0);
    }
    for (const Set &set : sets) {
        Rules r;
        r.draw = set.draw;
        r.passes = set.passes;
        const int users = deals / 4 > 0 ? deals / 4 : 1;
        int found = 0;
        std::uint32_t max_tries = 0;
        double slowest = 0, total = 0;
        for (int u = 1; u <= users; ++u) {
            const double t0 = now_s();
            const WinnableDeal w = find_winnable_deal(static_cast<std::uint32_t>(u), r, wl);
            const double dt = now_s() - t0;
            total += dt;
            if (dt > slowest) slowest = dt;
            if (w.found) ++found;
            if (w.tries > max_tries) max_tries = w.tries;
        }
        std::printf("winnable %-20s users %d (%u nodes x %u tries): found %d, most tries %u, avg %.1f ms, "
                    "slowest %.1f ms\n",
                    set.name, users, wl.nodes_per_try, wl.max_tries, found, max_tries, 1000.0 * total / users,
                    1000.0 * slowest);
    }
    return 0;
}
