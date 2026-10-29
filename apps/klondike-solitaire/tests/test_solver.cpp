/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the solver, the winnable deals, the hint, the dead-end check, and a random-playout fuzz with the
 * board's invariants (see test_core.cpp for the rest).
 */
#include "test_util.hpp"

#include "hint.hpp"
#include "solver.hpp"

#include <chrono>
#include <set>

using namespace klondike;

/* the moves replay through the game's own rules to a win */
static bool replays_to_win(const Board &start, const Rules &rules, const std::vector<Move> &moves)
{
    Game g;
    if (!g.new_game(start, rules)) return false;
    for (const Move &m : moves)
        if (!g.apply(m)) return false;
    return g.won();
}

static bool has_kind(const std::vector<Move> &moves, MoveKind k)
{
    for (const Move &m : moves)
        if (kind_of(m) == k) return true;
    return false;
}

/* A full deal built to be won with draw 1: every card comes free in order. */
static Board ordered_deal()
{
    return position("- - - -",
                    {"AC", "3c 2C", "6c 5c 4C", "tc 9c 8c 7C", "2d ad kc qc JC", "8d 7d 6d 5d 4d 3D",
                     "2h ah kd qd jd td 9D"},
                    "", "KS QS JS TS 9S 8S 7S 6S 5S 4S 3S 2S AS KH QH JH TH 9H 8H 7H 6H 5H 4H 3H", false);
}

/* 2C lies under 3C; both red fours are on the foundations; clubs 4..K in the stock. */
static Board buried_two()
{
    return position("C1 D13 H13 S13", {"2c 3C", "", "", "", "", "", ""}, "", "KC QC JC TC 9C 8C 7C 6C 5C 4C", false);
}

/* The same, but 4H can come down from the foundation onto 5C to take 3C. */
static Board needs_foundation_back()
{
    return position("C1 D13 H4 S13", {"2c 3C", "5C", "", "", "", "", ""}, "",
                    "KH QH JH TH 9H 8H 7H 6H 5H KC QC JC TC 9C 8C 7C 6C 4C", false);
}

/* Only back-and-forth moves are left: QC between the red Kings, Kings between columns; 3C (on the red Queens the
 * red Kings wait for) has nowhere to go. */
static Board only_shuffles()
{
    return position("C1 D11 H11 S13", {"qd qh 2c 3C", "KD", "KH", "QC", "KC", "jc tc 9c 8c 7c 6c 5c 4C", ""}, "", "",
                    false);
}

static void test_solver_constructed()
{
    Rules r;
    Solver solver;
    /* winnable */
    const Board ordered = ordered_deal();
    SolveResult res = solver.solve(ordered, r);
    CHECK(res.verdict == Verdict::Winnable && replays_to_win(ordered, r, res.solution));
    /* not winnable: 2C buried, no red four can come back */
    Rules no_back;
    no_back.foundation_back = false;
    res = solver.solve(buried_two(), no_back);
    CHECK(res.verdict == Verdict::NotWinnable && res.solution.empty());
    /* ... but winnable when any card may fill an empty column (3C moves away) */
    Rules any = no_back;
    any.fill = TableauFill::AnyCard;
    res = solver.solve(buried_two(), any);
    CHECK(res.verdict == Verdict::Winnable && replays_to_win(buried_two(), any, res.solution));
    /* a win that needs a card back from a foundation, and the same position without that rule */
    res = solver.solve(needs_foundation_back(), r);
    CHECK(res.verdict == Verdict::Winnable && replays_to_win(needs_foundation_back(), r, res.solution));
    CHECK(has_kind(res.solution, MoveKind::FoundationToTableau));
    res = solver.solve(needs_foundation_back(), no_back);
    CHECK(res.verdict == Verdict::NotWinnable);
    res = solver.solve(only_shuffles(), no_back);
    CHECK(res.verdict == Verdict::NotWinnable);
    /* an already won board */
    Board won = position("C13 D13 H13 S13", {"", "", "", "", "", "", ""}, "", "", false);
    res = solver.solve(won, r);
    CHECK(res.verdict == Verdict::Winnable && res.solution.empty());
    /* a budget too small to decide */
    SolverLimits tiny;
    tiny.max_nodes = 5;
    Solver small(tiny);
    res = small.solve(deal_board(3), r);
    CHECK(res.verdict == Verdict::Unknown && res.nodes <= 5);
    CHECK(std::string(verdict_name(Verdict::NotWinnable)) == "not winnable");
}

static void test_solver_deals()
{
    Solver solver;
    int verdicts[3] = {};
    std::uint64_t nodes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int draw = 1; draw <= 3; draw += 2) {
        Rules r;
        r.draw = draw;
        for (std::uint32_t d = 1; d <= 40; ++d) {
            const Board b = deal_board(d);
            const SolveResult a = solver.solve(b, r);
            nodes += a.nodes;
            ++verdicts[static_cast<int>(a.verdict)];
            if (a.verdict == Verdict::Winnable) CHECK(replays_to_win(b, r, a.solution));
            if (d <= 10) { /* deterministic: same answer, same work, same solution */
                const SolveResult again = solver.solve(b, r);
                CHECK(again.verdict == a.verdict && again.nodes == a.nodes && again.solution.size() == a.solution.size());
            }
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("solver: 80 deals: %d winnable, %d not winnable, %d unknown; %.0f nodes/s\n", verdicts[0], verdicts[1],
                verdicts[2], s > 0 ? nodes / s : 0.0);
    CHECK(verdicts[0] >= 50); /* most deals are won within the budget */
}

static void test_winnable_deals()
{
    Rules r;
    const WinnableDeal a = find_winnable_deal(7, r);
    const WinnableDeal b = find_winnable_deal(7, r);
    CHECK(a.found && b.found && a.deal == b.deal && a.tries == b.tries && a.nodes == b.nodes);
    CHECK(a.found && replays_to_win(deal_board(a.deal), r, a.solution));
    CHECK(winnable_candidate(7, 0) == 7 && winnable_candidate(7, 1) != 7 && winnable_candidate(7, 1) == winnable_candidate(7, 1));
    for (int draw = 1; draw <= 3; draw += 2) {
        Rules rr;
        rr.draw = draw;
        int found = 0;
        for (std::uint32_t u = 1; u <= 20; ++u) {
            const WinnableDeal w = find_winnable_deal(u, rr);
            if (w.found && replays_to_win(deal_board(w.deal), rr, w.solution)) ++found;
        }
        CHECK(found == 20);
    }
    /* a deal proven directly keeps its own number */
    for (std::uint32_t u = 1; u <= 10; ++u) {
        SolverLimits lim;
        lim.max_nodes = WinnableLimits().nodes_per_try;
        Solver s(lim);
        if (s.solve(deal_board(u), r).verdict == Verdict::Winnable) CHECK(find_winnable_deal(u, r).deal == u);
    }
}

/* ------------------------------------------------------------------------------------------------- hint */

static std::uint64_t layout_key(const Board &b)
{
    Board c = b;
    c.recycles = 0; /* a stock cycled once more is the same position for the player */
    return c.hash();
}

static void test_hint()
{
    Rules r;
    /* a hint is always legal */
    for (std::uint32_t d = 0; d < 50; ++d) {
        Game g;
        g.new_game(d, r);
        const Move h = hint(g);
        CHECK(h.count == 0 || g.can_apply(h));
    }
    /* a safe move to a foundation first */
    Game a;
    CHECK(a.new_game(position("- - - -", {"8d 9S", "TH", "AH", "", "", "", ""}, "8H", "", true), r));
    const Move h = hint(a);
    CHECK(h.from == T(3) && kind_of(h) == MoveKind::TableauToFoundation);
    /* a card taken back from a foundation is not sent straight back up: the card it was taken for goes on it */
    Game b;
    CHECK(b.new_game(position("C2 D13 H5 S13", {"6C", "", "", "", "", "", ""}, "4C", "", true), r));
    CHECK(kind_of(hint(b)) == MoveKind::Draw); /* 3C is in the stock and can go up: worth more than 5H down */
    CHECK(b.apply(mv(F(3), T(1), 1)));
    const Move after = hint(b);
    CHECK(after != mv(T(1), F(3), 1));
    CHECK(after == mv(kWaste, T(1), 1));
    /* no useful move: no hint (not an endless "draw") */
    Rules no_back;
    no_back.foundation_back = false;
    Game c;
    CHECK(c.new_game(buried_two(), no_back));
    CHECK(kind_of(hint(c)) == MoveKind::Draw); /* towards KC for the empty column */
    while (c.board().top(kWaste) != make_card(Clubs, 13)) CHECK(c.apply(c.stock_move()));
    CHECK(hint(c) == mv(kWaste, T(2), 1));
    CHECK(c.apply(mv(kWaste, T(2), 1)));
    CHECK(hint(c).count == 0);
    Game s;
    CHECK(s.new_game(only_shuffles(), no_back));
    CHECK(hint(s).count == 0);

    /* following the hints never comes back to a position */
    int games = 0, wins = 0, repeats = 0;
    for (int draw = 1; draw <= 3; draw += 2) {
        Rules rr;
        rr.draw = draw;
        for (std::uint32_t d = 1; d <= 150; ++d) {
            Game g;
            g.new_game(d, rr);
            std::set<std::uint64_t> seen{layout_key(g.board())};
            for (int step = 0; step < 3000; ++step) {
                const Move m = hint(g);
                if (!m.count) break;
                if (!g.apply(m)) {
                    CHECK(false);
                    break;
                }
                if (!seen.insert(layout_key(g.board())).second) {
                    ++repeats;
                    std::fprintf(stderr, "hint: deal %u draw %d comes back after %s (step %d)\n", d, draw,
                                 move_name(m).c_str(), step);
                    break;
                }
                if (g.won()) break;
            }
            ++games;
            wins += g.won();
        }
    }
    CHECK(repeats == 0);
    std::printf("hint: following the hints wins %d of %d deals (draw 1 and 3)\n", wins, games);
}

/* ------------------------------------------------------------------------------------------------- dead end */

static void test_dead_end()
{
    Rules no_back;
    no_back.foundation_back = false;
    Game g;
    CHECK(g.new_game(buried_two(), no_back));
    CHECK(!g.dead_end()); /* KC can still come out of the stock */
    while (g.board().top(kWaste) != make_card(Clubs, 13)) g.apply(g.stock_move());
    CHECK(g.apply(mv(kWaste, T(2), 1)));
    CHECK(g.dead_end()); /* only endless stock cycling */
    Game s;
    CHECK(s.new_game(only_shuffles(), no_back));
    CHECK(!s.legal_moves().empty() && s.dead_end()); /* moves left, all pointless: the solver proves it */
    Rules any = no_back;
    any.fill = TableauFill::AnyCard;
    Game a;
    CHECK(a.new_game(only_shuffles(), any));
    CHECK(!a.dead_end()); /* 3C may go to the empty column */
    Game w;
    CHECK(w.new_game(needs_foundation_back(), Rules()));
    CHECK(!w.dead_end());
    for (std::uint32_t d = 0; d < 30; ++d) {
        Game n;
        n.new_game(d, Rules());
        CHECK(!n.dead_end());
    }
    Game won;
    CHECK(won.new_game(position("C13 D13 H13 S13", {"", "", "", "", "", "", ""}, "", "", false), Rules()));
    CHECK(won.won() && !won.dead_end());
}

/* ------------------------------------------------------------------------------------------------- fuzz */

static void test_fuzz()
{
    TestRng rng(2026);
    Game g;
    int actions = 0, deals = 0, invalid = 0, wins = 0;
    std::uint32_t deal = 1000;
    while (actions < 10000) {
        Rules r;
        r.draw = rng.below(2) ? 3 : 1;
        const int passes[] = {0, 3, 1};
        r.passes = passes[rng.below(3)];
        r.foundation_back = rng.below(2) != 0;
        r.fill = rng.below(4) ? TableauFill::KingOnly : TableauFill::AnyCard;
        r.scoring = static_cast<Scoring>(rng.below(3));
        g.new_game(deal++, r);
        ++deals;
        for (int i = 0; i < 300 && actions < 10000; ++i) {
            const std::uint32_t roll = rng.below(100);
            const Board before = g.board();
            if (roll < 8 && g.can_undo()) {
                CHECK(g.undo());
                if (g.can_redo() && rng.below(2)) {
                    CHECK(g.redo() && g.board() == before);
                    CHECK(g.undo());
                }
            } else if (roll < 12 && g.can_redo()) {
                CHECK(g.redo());
            } else {
                /* prefer moves over the stock so that games get somewhere */
                const std::vector<Move> moves = g.legal_moves();
                if (moves.empty()) break;
                Move m = moves[rng.below(static_cast<std::uint32_t>(moves.size()))];
                if (moves.size() > 1 && (kind_of(m) == MoveKind::Draw || kind_of(m) == MoveKind::Recycle) && rng.below(2))
                    m = moves[1 + rng.below(static_cast<std::uint32_t>(moves.size() - 1))];
                CHECK(g.apply(m));
                for (Move a = g.safe_auto_move(); a.count && rng.below(2); a = g.safe_auto_move()) CHECK(g.apply(a, true));
            }
            ++actions;
            std::string why;
            if (!g.board().valid(&why)) {
                if (++invalid < 5) std::fprintf(stderr, "fuzz: deal %u: %s\n", deal - 1, why.c_str());
            }
            for (const Move &m : g.legal_moves())
                if (!g.can_apply(m)) ++invalid;
            if (g.score() < 0 && r.scoring == Scoring::Standard) ++invalid;
            if (r.scoring == Scoring::Vegas && g.score() != score::vegas(g.board().foundation_cards())) ++invalid;
            if (g.won()) {
                ++wins;
                break;
            }
        }
        Board back;
        if (!Board::from_text(g.board().to_text(), back) || back != g.board()) ++invalid;
    }
    CHECK(invalid == 0);
    CHECK(actions == 10000);
    std::printf("fuzz: %d actions over %d deals, %d won, every invariant held: %s\n", actions, deals, wins,
                invalid ? "no" : "yes");
}

void test_solver_group()
{
    test_solver_constructed();
    test_solver_deals();
    test_winnable_deals();
    test_hint();
    test_dead_end();
    test_fuzz();
}
