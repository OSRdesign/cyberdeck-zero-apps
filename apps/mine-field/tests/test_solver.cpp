/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the solver, the hints and the fair-board generator (core/solver.hpp, core/generator.hpp). The solver is
 * checked against the TRUE mine layout (it must never be wrong) and against an independent brute-force oracle over the
 * frontier cells (it must find what local logic proves). No LVGL.
 */
#include "test_util.hpp"

#include "generator.hpp"
#include "solver.hpp"

#include <algorithm>
#include <chrono>
#include <set>

using namespace minefield;

namespace {

/* A view from text: '#' hidden, 'F' flagged, '0'..'8' revealed. */
View view_of(const std::vector<std::string> &rows, int mines)
{
    View v;
    v.h = static_cast<int>(rows.size());
    v.w = static_cast<int>(rows[0].size());
    v.mines = mines;
    for (const std::string &r : rows)
        for (char c : r) v.cell.push_back(c == '#' ? kViewHidden : c == 'F' ? kViewFlagged : c == '.' ? 0 : static_cast<std::int8_t>(c - '0'));
    return v;
}

std::set<int> as_set(const std::vector<int> &v)
{
    return std::set<int>(v.begin(), v.end());
}

/* Plays a random partial game: reveal the first click, then `steps` random safe cells by the true layout. */
Board random_state(const Dims &d, std::uint64_t seed, int steps, TestRng &rng, bool until_stuck = false,
                   int *first_out = nullptr)
{
    Board b(d.w, d.h, d.mines);
    const int first = static_cast<int>(rng.below(static_cast<std::uint32_t>(d.w * d.h)));
    b.place(place_mines(d.w, d.h, d.mines, seed, first));
    b.reveal(first);
    if (first_out) *first_out = first;
    for (int s = 0; s < steps && !b.all_safe_revealed(); ++s) {
        std::vector<int> hid;
        for (int i = 0; i < b.size(); ++i)
            if (b.state(i) == CellState::Hidden && !b.is_mine(i)) hid.push_back(i);
        if (hid.empty()) break;
        b.reveal(hid[rng.below(static_cast<std::uint32_t>(hid.size()))]);
    }
    if (until_stuck) {
        /* then follow the solver until it has nothing left: the states a player stuck without a guess sees */
        for (int c; (c = find_safe_cell(b.view())) >= 0;)
            if (b.reveal(c) != RevealResult::Opened) break;   /* (a wrong solver would loop for ever here) */
    }
    return b;
}

/* ---- the oracle: every assignment of the frontier cells that fits all the numbers (flags count as unknown, the global
 * mine count is ignored); the cells that are safe / mines in all of them. false when the frontier is too big. */
bool oracle(const View &v, std::set<int> &safe, std::set<int> &mines)
{
    std::vector<int> front;
    std::vector<int> id(static_cast<std::size_t>(v.size()), -1);
    struct Num {
        int count;
        std::vector<int> cells;   /* frontier ids */
    };
    std::vector<Num> nums;
    for (int i = 0; i < v.size(); ++i) {
        if (v.cell[static_cast<std::size_t>(i)] < 0) continue;
        Num n;
        n.count = v.cell[static_cast<std::size_t>(i)];
        const int x = i % v.w, y = i / v.w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= v.w || ny >= v.h) continue;
                const int j = ny * v.w + nx;
                if (v.cell[static_cast<std::size_t>(j)] >= 0) continue;
                if (id[static_cast<std::size_t>(j)] < 0) {
                    id[static_cast<std::size_t>(j)] = static_cast<int>(front.size());
                    front.push_back(j);
                }
                n.cells.push_back(id[static_cast<std::size_t>(j)]);
            }
        if (!n.cells.empty()) nums.push_back(n);
    }
    if (front.size() > 17) return false;
    const std::uint32_t total = 1u << front.size();
    std::vector<std::uint32_t> masks;
    for (const Num &n : nums) {
        std::uint32_t m = 0;
        for (int c : n.cells) m |= 1u << c;
        masks.push_back(m);
    }
    std::uint32_t always_mine = ~0u, ever_mine = 0;
    int solutions = 0;
    for (std::uint32_t a = 0; a < total; ++a) {
        bool ok = true;
        for (std::size_t k = 0; k < nums.size() && ok; ++k)
            ok = __builtin_popcount(a & masks[k]) == nums[k].count;
        if (!ok) continue;
        ++solutions;
        always_mine &= a;
        ever_mine |= a;
    }
    safe.clear();
    mines.clear();
    if (!solutions) return true;
    for (std::size_t k = 0; k < front.size(); ++k) {
        if (!((ever_mine >> k) & 1u)) safe.insert(front[k]);
        else if ((always_mine >> k) & 1u) mines.insert(front[k]);
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ hand-made */

void test_rules()
{
    /* a chain: the mine at (1,0) is told by 1 1 1 0 0 under a hidden row; the zeros give the safe cells at once, then
     * the ones settle (0,0) */
    const View v = view_of({"#####", "11100", "00000", "00000", "00000"}, 1);
    Deduction d;
    deduce(v, d);
    CHECK(as_set(d.safe) == (std::set<int>{0, 2, 3, 4}));
    CHECK(as_set(d.mines) == (std::set<int>{1}));
    CHECK(find_mine(v) == 1);
    const int s = find_safe_cell(v);
    CHECK(s == 0 || s == 2 || s == 3 || s == 4);

    /* rule 1: a corner 1 with one hidden neighbour; a 2 in the corner with two */
    {
        const View a = view_of({"1#100", "11100", "00000", "00000", "00000"}, 1);
        CHECK(find_mine(a) == 1);
        Deduction da;
        deduce(a, da);
        CHECK(da.safe.empty() && as_set(da.mines) == (std::set<int>{1}));
    }
    /* rule 2 (subset): 1 over {a,b} and 1 over {a,b,c} -> c is safe; 1 over {a,b} and 2 over {a,b,c} -> c is a mine.
     * Left edge, numbers at x = 1 over hidden cells at x = 0 (no zero is revealed anywhere near, so only the subset
     * rule can work): (0,1),(0,2),(0,3) hidden, numbers (1,1)=?, (1,2)=?, (1,3)=?  with rows 0 and 4 revealed and open */
    {
        /* mines (0,2) and (0,3): number (1,1) sees (0,1)? */
        const View a = view_of({"#1...", "#1...", "#2...", "#1...", "#1..."}, 2);
        /* (1,0)=1 sees {(0,0),(0,1)}; (1,1)=1 sees {(0,0),(0,1),(0,2)} -> (0,2) safe; (1,2)=2 sees {(0,1),(0,2),(0,3)};
         * (1,3)=1 sees {(0,2),(0,3),(0,4)}; (1,4)=1 sees {(0,3),(0,4)} -> (0,2) is safe, so (1,2) says (0,1),(0,3) mines */
        Deduction da;
        deduce(a, da);
        CHECK(as_set(da.safe) == (std::set<int>{2 * 5, 0, 4 * 5}));
        CHECK(as_set(da.mines) == (std::set<int>{5, 15}));
    }
    /* flags are not trusted: a wrong flag on a safe cell does not hide it, a flag on a mine is still reported */
    {
        const View a = view_of({"#F100", "11100", "00000", "00000", "00000"}, 1);
        Deduction da;
        deduce(a, da);
        CHECK(as_set(da.mines) == (std::set<int>{1}) && as_set(da.safe) == (std::set<int>{0}));   /* the flagged mine: proven */
        CHECK(find_mine(a) == -1);                        /* but already flagged: nothing to hint */
        const View c = view_of({"F###0", "11100", "00000", "00000", "00000"}, 1);
        Deduction dc;
        deduce(c, dc);
        CHECK(as_set(dc.safe) == (std::set<int>{0, 2, 3}) && as_set(dc.mines) == (std::set<int>{1}));   /* (0,0) flagged wrongly: still proven safe */
        CHECK(find_safe_cell(c) == 2);                      /* an unflagged one is preferred */
        const View e = view_of({"F#100", "11100", "00000", "00000", "00000"}, 1);
        CHECK(find_safe_cell(e) == 0);                      /* only a flagged cell is safe: still reported */
    }
    /* rule 3: the whole board count */
    {
        const View a = view_of({"#####", "#####", "#####", "#####", "####0"}, 0);
        Deduction da;
        deduce(a, da);
        CHECK(da.safe.size() == 24);   /* the zero opens three, then no mine is left: all of them */
        const View b = view_of({"#####", "#####", "#####", "#####", "#####"}, 25);
        Deduction db;
        deduce(b, db);
        CHECK(db.mines.size() == 25 && db.safe.empty());
    }
    /* a view with nothing to say */
    {
        const View a = view_of({"#####", "#####", "#####", "#####", "#####"}, 3);
        CHECK(find_safe_cell(a) == -1 && find_mine(a) == -1);
        Deduction da;
        deduce(a, da);
        CHECK(da.safe.empty() && da.mines.empty() && !da.exhausted);
        View bad;
        deduce(bad, da);
        CHECK(da.safe.empty());
    }
}

void test_budget()
{
    TestRng brng(9);
    Board b = random_state(Dims{30, 16, 99}, 5, 60, brng);
    Budget tiny;
    tiny.limit = 3;
    Deduction d;
    deduce(b.view(), d, &tiny);
    CHECK(d.exhausted && tiny.exhausted() && tiny.used > 3);
    Budget big;
    big.limit = 100000000;
    deduce(b.view(), d, &big);
    CHECK(!d.exhausted && big.used > 0);
    /* solvability under a tiny budget is a no on any board; unlimited is the same as a big one */
    Board p(30, 16, 99);
    p.place(place_mines(30, 16, 99, 1, 100));
    Budget t2;
    t2.limit = 10;
    CHECK(!is_solvable_without_guessing(p, 100, &t2) && t2.exhausted());
    int rounds = 0;
    Budget u;
    Budget v2;
    v2.limit = 100000000;
    CHECK(is_solvable_without_guessing(p, 100, &u, &rounds) == is_solvable_without_guessing(p, 100, &v2));
    CHECK(rounds >= 1);
    /* a bad first click / a board without layout */
    CHECK(!is_solvable_without_guessing(p, -1) && !is_solvable_without_guessing(p, 480));
    Board unplaced(9, 9, 10);
    CHECK(!is_solvable_without_guessing(unplaced, 0));
    int mine_cell = 0;
    while (!p.is_mine(mine_cell)) ++mine_cell;
    CHECK(!is_solvable_without_guessing(p, mine_cell));
}

/* ------------------------------------------------------------------------------------------------ random boards */

void test_soundness_and_completeness()
{
    TestRng rng(2024);
    int states = 0, with_safe = 0, with_mine = 0, none = 0, oracle_states = 0, enum_gain = 0, flagged_states = 0;
    for (const Dims d : {Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}, Dims{8, 8, 25}, Dims{6, 6, 8}}) {
        for (int s = 0; s < 120; ++s) {
            const int steps = static_cast<int>(rng.below(40));
            Board b = random_state(d, rng.next(), steps, rng, s % 3 == 0);
            const View v = b.view();
            Deduction dd;
            deduce(v, dd);
            ++states;
            /* soundness against the TRUE layout */
            for (int c : dd.safe) CHECK(!b.is_mine(c) && b.state(c) == CellState::Hidden);
            for (int c : dd.mines) CHECK(b.is_mine(c) && b.state(c) == CellState::Hidden);
            const int fs = find_safe_cell(v), fm = find_mine(v);
            if (fs >= 0) {
                ++with_safe;
                CHECK(!b.is_mine(fs) && b.state(fs) == CellState::Hidden);
            }
            if (fm >= 0) {
                ++with_mine;
                CHECK(b.is_mine(fm));
            }
            CHECK((fs >= 0) == !dd.safe.empty());   /* the single-cell search agrees with the full one */
            if (fs < 0) ++none;   /* nothing safe provable (a mine may be) */
            /* completeness for local logic: whatever the brute-force oracle proves, the solver proves too */
            std::set<int> osafe, omines;
            if (oracle(v, osafe, omines)) {
                ++oracle_states;
                const std::set<int> ds = as_set(dd.safe), dm = as_set(dd.mines);
                for (int c : osafe) CHECK(ds.count(c) == 1);
                for (int c : omines) CHECK(dm.count(c) == 1);
                /* "none provable" only when the oracle really has nothing either (the global count is the one extra) */
                if (fs < 0) CHECK(osafe.empty());
                for (int c : ds) CHECK(!b.is_mine(c));
            }
            /* rule 4 does work: with it off, fewer cells */
            Budget off;
            off.enum_limit = 0;
            Deduction d0;
            deduce(v, d0, &off);
            for (int c : d0.safe) CHECK(!b.is_mine(c));
            for (int c : d0.mines) CHECK(b.is_mine(c));
            if (dd.safe.size() + dd.mines.size() > d0.safe.size() + d0.mines.size()) ++enum_gain;
            /* wrong flags change nothing but the preference: same sets with random flags (right and wrong) */
            View f = v;
            for (int i = 0; i < f.size(); ++i)
                if (f.cell[static_cast<std::size_t>(i)] == kViewHidden && rng.below(5) == 0) f.cell[static_cast<std::size_t>(i)] = kViewFlagged;
            Deduction df;
            deduce(f, df);
            CHECK(as_set(df.safe) == as_set(dd.safe) && as_set(df.mines) == as_set(dd.mines));
            ++flagged_states;
            const int ffs = find_safe_cell(f);
            if (ffs >= 0) CHECK(!b.is_mine(ffs));
        }
    }
    std::printf("solver: %d states, %d with a safe cell, %d with a mine, %d with no safe cell provable, %d oracle-checked, %d where rule 4 adds, %d flagged\n",
                states, with_safe, with_mine, none, oracle_states, enum_gain, flagged_states);
    CHECK(with_safe > 100 && none > 50 && oracle_states > 100);
    CHECK(enum_gain > 0);   /* the enumeration step finds things the plain rules do not */
}

void test_hint_through_game()
{
    /* a hint is only given when provable; a "none" costs nothing; a given hint cell is truly safe or a mine */
    TestRng rng(77);
    int safe_hints = 0, mine_hints = 0, nones = 0;
    for (int s = 0; s < 300; ++s) {
        const Dims d = s % 3 == 0 ? Dims{9, 9, 10} : s % 3 == 1 ? Dims{16, 16, 40} : Dims{30, 16, 99};
        Game g(GameConfig{d, rng.next(), false});
        g.reveal(static_cast<int>(rng.below(static_cast<std::uint32_t>(d.w))), static_cast<int>(rng.below(static_cast<std::uint32_t>(d.h))));
        for (int step = 0; step < 25 && g.status() == Game::Status::Playing; ++step) {
            const View v = g.view();
            const int fs = find_safe_cell(v), fm = find_mine(v);
            const int before = g.hints_used();
            const std::uint32_t pen = g.penalty_ms();
            const Game::Hint h = g.hint();
            if (fs >= 0) {
                CHECK(h.kind == Game::HintKind::Safe && !g.board().is_mine(h.cell) && h.cell == fs);
                ++safe_hints;
            } else if (fm >= 0) {
                CHECK(h.kind == Game::HintKind::Mine && g.board().is_mine(h.cell));
                ++mine_hints;
            } else {
                CHECK(h.kind == Game::HintKind::None && g.hints_used() == before && g.penalty_ms() == pen);
                ++nones;
            }
            if (h.kind != Game::HintKind::None) CHECK(g.hints_used() == before + 1 && g.penalty_ms() == pen + 10000);
            /* continue with a random safe cell */
            std::vector<int> hid;
            for (int i = 0; i < g.board().size(); ++i)
                if (g.board().state(i) == CellState::Hidden && !g.board().is_mine(i)) hid.push_back(i);
            if (hid.empty()) break;
            const int c = hid[rng.below(static_cast<std::uint32_t>(hid.size()))];
            g.reveal(c % d.w, c / d.w);
        }
    }
    std::printf("hints: %d safe, %d mine, %d none\n", safe_hints, mine_hints, nones);
    CHECK(safe_hints > 100 && nones > 10);
}

/* ------------------------------------------------------------------------------------------------ fair boards */

/* Opens the whole board with find_safe_cell alone; true when it wins without ever hitting a mine. */
bool solve_with_hints(Game &g)
{
    for (int step = 0; step < 5000 && g.status() == Game::Status::Playing; ++step) {
        const int c = find_safe_cell(g.view());
        if (c < 0) return false;
        if (g.reveal(c % g.width(), c / g.width()) == Game::Outcome::Exploded) return false;
    }
    return g.status() == Game::Status::Won;
}

void test_fair_generation()
{
    int verified = 0, failed = 0;
    for (const Dims d : {Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}, Dims{5, 5, 16}, Dims{12, 7, 25}}) {
        const int count = d.w == 30 ? 12 : 60;
        for (int s = 1; s <= count; ++s) {
            const std::uint64_t seed = 1000 + static_cast<std::uint64_t>(s) * 31;
            const int fx = s % d.w, fy = (s * 3) % d.h;
            Game g(GameConfig{d, seed, true});
            CHECK(g.reveal(fx, fy) != Game::Outcome::Exploded);
            CHECK(g.attempts() >= 1 && g.attempts() <= kFairMaxAttempts);
            const GenResult r = generate(d.w, d.h, d.mines, seed, fy * d.w + fx, true);
            CHECK(r.mines == g.board().mine_map() && r.attempts == g.attempts() && r.fair_failed == g.fair_failed());
            if (g.fair_failed()) {
                ++failed;
                CHECK(d.w == 5 || d.w == 12 || d.w == 30);   /* only the very dense boards (and Expert's budget) may fail */
                CHECK(g.attempts() == kFairMaxAttempts || r.solver_work > kFairWorkBudget);
                continue;
            }
            /* independent check: a fresh game on the same board, played by find_safe_cell alone, wins */
            Game replay(GameConfig{d, seed, true});
            replay.reveal(fx, fy);
            CHECK(replay.board().mine_map() == g.board().mine_map());
            CHECK(solve_with_hints(replay));
            CHECK(replay.hints_used() == 0);
            ++verified;
        }
    }
    std::printf("fair: %d boards verified by independent solve, %d fair_failed\n", verified, failed);
    CHECK(verified > 150);

    /* the check is not vacuous: plain boards are often NOT solvable without a guess, and some are */
    for (const Dims d : {Dims{9, 9, 10}, Dims{30, 16, 99}}) {
        int yes = 0, no = 0;
        for (std::uint64_t seed = 1; seed <= 150; ++seed) {
            Board b(d.w, d.h, d.mines);
            const int first = static_cast<int>(seed * 17 % static_cast<std::uint64_t>(d.w * d.h));
            b.place(place_mines(d.w, d.h, d.mines, seed, first));
            const bool ok = is_solvable_without_guessing(b, first);
            Game g(GameConfig{d, seed, false});
            g.reveal(first % d.w, first / d.w);
            CHECK(g.board().mine_map() == b.mine_map());
            CHECK(solve_with_hints(g) == ok);   /* both ways of asking agree */
            (ok ? yes : no)++;
        }
        std::printf("plain %dx%d/%d: %d of 150 boards solvable without guessing\n", d.w, d.h, d.mines, yes);
        CHECK(no > 0);
        if (d.w == 9) CHECK(yes > 0);
    }
    /* a fair game that failed still plays (the last board); fair=false is a single instant attempt */
    GenOptions tiny;
    tiny.max_attempts = 1;
    const GenResult f1 = generate(30, 16, 99, 99, 0, true, tiny);
    CHECK(f1.attempts == 1 && static_cast<int>(f1.mines.size()) == 480);
    const GenResult plain = generate(30, 16, 99, 99, 0, false);
    CHECK(plain.attempts == 1 && !plain.fair_failed && plain.solver_work == 0 && plain.mines == f1.mines);
    GenOptions starved;
    starved.work_budget = 1;
    const GenResult f2 = generate(30, 16, 99, 5, 0, true, starved);
    CHECK(f2.fair_failed && f2.attempts <= 2);
}

void test_generation_time()
{
    for (const Dims d : {Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}}) {
        double total = 0, worst = 0;
        long att = 0, work_total = 0, work_max = 0;
        int max_att = 0, failed = 0;
        const int n = 20;
        for (int s = 0; s < n; ++s) {
            const auto t0 = std::chrono::steady_clock::now();
            const GenResult r = generate(d.w, d.h, d.mines, 424200 + static_cast<std::uint64_t>(s) * 7919,
                                         (s * 37) % (d.w * d.h), true);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms;
            worst = std::max(worst, ms);
            att += r.attempts;
            work_total += r.solver_work;
            work_max = std::max(work_max, r.solver_work);
            max_att = std::max(max_att, r.attempts);
            failed += r.fair_failed ? 1 : 0;
            CHECK(ms < 10000.0);   /* very generous: the real bound is the work budget */
        }
        std::printf("fair %2dx%2d/%2d, %d seeds: avg %.1f ms, worst %.1f ms, attempts avg %.1f max %d, fair_failed %d, work avg %ld max %ld\n",
                    d.w, d.h, d.mines, n, total / n, worst, static_cast<double>(att) / n, max_att, failed, work_total / n, work_max);
    }
    /* the worst case: boards that are (almost) never fair run until the attempt cap or the work budget ends the search */
    for (const Dims d : {Dims{30, 16, 99}, Dims{30, 30, 200}, Dims{30, 30, 400}, Dims{16, 16, 90}, Dims{5, 5, 16}}) {
        const auto t0 = std::chrono::steady_clock::now();
        const GenResult r = generate(d.w, d.h, d.mines, 31337, (d.w * d.h) / 2 + d.w / 2, true);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("fair worst case %2dx%2d/%3d: %.1f ms, attempts %d, work %ld, fair_failed %d\n", d.w, d.h, d.mines, ms,
                    r.attempts, r.solver_work, r.fair_failed ? 1 : 0);
        CHECK(ms < 10000.0);
    }
}

} // namespace

void test_solver_group()
{
    test_rules();
    test_budget();
    test_soundness_and_completeness();
    test_hint_through_game();
    test_fair_generation();
    test_generation_time();
}
