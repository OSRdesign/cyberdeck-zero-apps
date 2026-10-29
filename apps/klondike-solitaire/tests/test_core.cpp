/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of Klondike's core/ (no LVGL, no screen) and of its package files (.desktop, app.json).
 * This file: cards, the deal, the board text, every move type, sequence moves, undo / redo, the scoring table, the
 * game's counters. test_solver.cpp: solver, winnable deals, hint, dead end, random fuzz. test_store.cpp: JSON, rules,
 * statistics, saved game, files. Built and run by ../../_common/build_app.sh host (make -C _common/headless).
 */
#include "test_util.hpp"

#include "board.hpp"
#include "game.hpp"
#include "rules.hpp"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

using namespace klondike;

/* The deal generator, pinned (board.hpp): layout_checksum() of four deals and the first two columns of deal 1. */
constexpr std::uint32_t PIN_DEAL_1 = 0x58BEDB81u;
constexpr std::uint32_t PIN_DEAL_2 = 0xC0A556FBu;
constexpr std::uint32_t PIN_DEAL_123456 = 0xCBD68E29u;
constexpr std::uint32_t PIN_DEAL_MAX = 0xE0251DFDu;
static const char PIN_DEAL_1_TEXT[] = "f - - - -\nt AD\nt kh 8S"; /* the first two columns */

static std::string read_text(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Board position(const std::string &found, const std::vector<std::string> &cols, const std::string &waste,
               const std::string &stock, bool rest_to_stock, int recycles)
{
    bool used[kCards] = {};
    auto mark = [&used](const std::string &list) {
        std::istringstream in(list);
        for (std::string w; in >> w;) {
            Card c;
            if (parse_card(w, c)) used[c] = true;
        }
    };
    {
        std::istringstream in(found);
        for (std::string w; in >> w;) {
            if (w == "-" || w.size() < 2) continue;
            const std::string suits = "CDHS";
            const std::size_t s = suits.find(w[0]);
            const int n = std::atoi(w.c_str() + 1);
            if (s != std::string::npos)
                for (int r = 1; r <= n && r <= kRanks; ++r) used[make_card(static_cast<int>(s), r)] = true;
        }
    }
    for (const std::string &c : cols) mark(c);
    mark(waste);
    mark(stock);
    std::string rest;
    if (rest_to_stock)
        for (int i = 0; i < kCards; ++i)
            if (!used[i]) rest += card_name(static_cast<Card>(i)) + " ";
    std::string text = "f " + found + "\n";
    for (const std::string &c : cols) text += "t " + c + "\n";
    text += "w " + waste + "\ns " + rest + stock + "\nr " + std::to_string(recycles) + "\n";
    Board b;
    std::string why;
    const bool ok = Board::from_text(text, b, &why);
    if (!ok) std::fprintf(stderr, "position: %s\n%s\n", why.c_str(), text.c_str());
    CHECK(ok);
    return b;
}

/* ------------------------------------------------------------------------------------------------- cards */

static void test_cards()
{
    CHECK(card_name(make_card(Spades, 1)) == "AS");
    CHECK(card_name(make_card(Hearts, 10)) == "TH");
    CHECK(card_name(make_card(Clubs, 12)) == "QC");
    CHECK(card_name(make_card(Diamonds, 7)) == "7D");
    Card c = 0;
    bool down = true;
    CHECK(parse_card("10H", c, &down) && c == make_card(Hearts, 10) && !down);
    CHECK(parse_card("th", c, &down) && c == make_card(Hearts, 10) && down);
    CHECK(!parse_card("aS", c));
    CHECK(!parse_card("XS", c));
    CHECK(!parse_card("A", c));
    CHECK(is_red(make_card(Diamonds, 3)) && is_red(make_card(Hearts, 3)));
    CHECK(!is_red(make_card(Clubs, 3)) && !is_red(make_card(Spades, 3)));
    CHECK(builds_on(make_card(Hearts, 6), make_card(Spades, 7)));
    CHECK(!builds_on(make_card(Hearts, 6), make_card(Diamonds, 7)));
    CHECK(!builds_on(make_card(Hearts, 6), make_card(Spades, 8)));
    CHECK(!builds_on(make_card(Spades, 13), make_card(Hearts, 1)));
    for (int i = 0; i < kCards; ++i) {
        Card back;
        CHECK(parse_card(card_name(static_cast<Card>(i)), back) && back == i);
    }
}

/* ------------------------------------------------------------------------------------------------- the deal */

static void test_deal()
{
    CHECK(deal_board(1) == deal_board(1));
    CHECK(deal_board(1) != deal_board(2));
    CHECK(deal_board(0) != deal_board(0xFFFFFFFFu));
    std::set<std::uint32_t> sums;
    int top_of_first[kCards] = {};
    for (std::uint32_t n = 0; n < 2000; ++n) {
        const Board b = deal_board(n);
        std::string why;
        if (!b.valid(&why)) std::fprintf(stderr, "deal %u: %s\n", n, why.c_str());
        CHECK(b.valid());
        bool shape = b.ntalon == 24 && b.tp == 0 && b.recycles == 0 && b.foundation_cards() == 0;
        for (int c = 0; c < kColumns; ++c) shape = shape && b.ntab[c] == c + 1 && b.ndown[c] == c;
        CHECK(shape);
        sums.insert(layout_checksum(b));
        ++top_of_first[b.tab[0][0]];
    }
    CHECK(sums.size() == 2000); /* 2000 different deals */
    int least = 1 << 30;
    for (int n : top_of_first) least = n < least ? n : least;
    CHECK(least >= 15); /* about 38 each: every card turns up as the first column's card */

    /* the generator is pinned: these deals must never change (saved games and shared deal numbers rely on them) */
    const Board d1 = deal_board(1);
    const std::uint32_t pins[][2] = {{1u, PIN_DEAL_1}, {2u, PIN_DEAL_2}, {123456u, PIN_DEAL_123456},
                                     {4294967295u, PIN_DEAL_MAX}};
    for (const auto &p : pins) {
        const std::uint32_t got = layout_checksum(deal_board(p[0]));
        if (got != p[1]) std::fprintf(stderr, "deal %u checksum 0x%08X, pinned 0x%08X\n", p[0], got, p[1]);
        CHECK(got == p[1]);
    }
    const std::string first_lines = d1.to_text().substr(0, d1.to_text().find("\nt", 20));
    if (first_lines != PIN_DEAL_1_TEXT) std::fprintf(stderr, "deal 1 starts:\n%s\n", first_lines.c_str());
    CHECK(first_lines == PIN_DEAL_1_TEXT);
}

/* ------------------------------------------------------------------------------------------------- board text */

static void test_board_text()
{
    for (std::uint32_t n = 0; n < 50; ++n) {
        const Board b = deal_board(n * 7919u);
        Board back;
        CHECK(Board::from_text(b.to_text(), back) && back == b);
    }
    Board b = position("C1 D13 H4 S13", {"2c 3C", "5C", "", "", "", "", ""}, "", "", true);
    CHECK(b.suit_count(Clubs) == 1 && b.suit_count(Hearts) == 4 && b.foundation_cards() == 31);
    Board back;
    CHECK(Board::from_text(b.to_text(), back) && back == b);
    CHECK(Board::from_text("f - - - -;t;t;t;t;t;t;t;s " + [] {
              std::string s;
              for (int i = 0; i < kCards; ++i) s += card_name(static_cast<Card>(i)) + " ";
              return s;
          }() + ";r 0",
                           back));
    CHECK(back.stock_size() == 52 && back.top(kStock) == make_card(Spades, 13));
    std::string why;
    CHECK(!Board::from_text("f - - - -", back, &why) && !why.empty());               /* no columns */
    CHECK(!Board::from_text("f C14 - - -;t;t;t;t;t;t;t", back, &why));              /* foundation over 13 */
    const std::string all_but_ks = [] { /* KS is id 51, the last */
        std::string s;
        for (int i = 0; i < kCards - 1; ++i) s += card_name(static_cast<Card>(i)) + " ";
        return s;
    }();
    CHECK(!Board::from_text("f - - - -;t;t;t;t;t;t;t;s " + all_but_ks + ";r 0", back, &why)); /* KS missing */
    CHECK(why.find("KS missing") != std::string::npos);
    CHECK(!Board::from_text("f - - - -;t;t;t;t;t;t;t;s " + all_but_ks + " AC;r 0", back, &why)); /* AC twice */
    CHECK(why.find("AC twice") != std::string::npos);
    CHECK(!Board::from_text("f - - - -;t KS qh;t;t;t;t;t;t", back, &why)); /* face down on face up */
    CHECK(why.find("face-down") != std::string::npos);
    CHECK(!Board::from_text("f S13 - - -;t 5D 9H;t;t;t;t;t;t", back, &why)); /* run out of sequence */
    CHECK(why.find("out of sequence") != std::string::npos);
    CHECK(!Board::from_text("f S13 - - -;t 5d;t;t;t;t;t;t", back, &why)); /* a face-down top */
    CHECK(why.find("face-up top") != std::string::npos);
    CHECK(!Board::from_text("f S13 S1 - -;t;t;t;t;t;t;t", back, &why)); /* one suit twice */
}

/* ------------------------------------------------------------------------------------------------- moves */

static void test_draw_and_recycle()
{
    Rules r1;
    Board b = position("- - - -", {"KS", "", "", "", "", "", ""}, "", "", true);
    CHECK(b.stock_size() == 51 && b.waste_size() == 0);
    CHECK(can_move(b, r1, mv(kStock, kWaste, 1)));
    CHECK(!can_move(b, r1, mv(kStock, kWaste, 3)));  /* draw 1 draws one */
    CHECK(!can_move(b, r1, mv(kWaste, kStock, 0)));  /* nothing to recycle, stock not empty */
    Rules r3;
    r3.draw = 3;
    CHECK(can_move(b, r3, mv(kStock, kWaste, 3)));
    CHECK(!can_move(b, r3, mv(kStock, kWaste, 1)));
    /* the last two cards: draw 3 takes 2 */
    Board two = position("C13 D13 H13 S11", {"", "", "", "", "", "", ""}, "", "KS QS", false);
    CHECK(can_move(two, r3, mv(kStock, kWaste, 2)) && !can_move(two, r3, mv(kStock, kWaste, 3)));
    do_move(two, r3, mv(kStock, kWaste, 2));
    CHECK(two.top(kWaste) == make_card(Spades, 13) && two.stock_size() == 0); /* the last drawn is on top */
    CHECK(!can_move(two, r3, mv(kStock, kWaste, 1)));                         /* empty stock */
    CHECK(can_move(two, r3, mv(kWaste, kStock, 2)));
    CHECK(!can_move(two, r3, mv(kWaste, kStock, 1)));                         /* recycle takes the whole waste */
    do_move(two, r3, mv(kWaste, kStock, 2));
    CHECK(two.top(kStock) == make_card(Spades, 12) && two.recycles == 1);    /* QS drawn first again */

    /* pass limits: 1 pass = no recycle, 3 passes = two */
    Board w = position("C13 D13 H13 S11", {"", "", "", "", "", "", ""}, "QS KS", "", false);
    Rules p1;
    p1.passes = 1;
    CHECK(!can_move(w, p1, mv(kWaste, kStock, 2)));
    CHECK(stock_move(w, p1).count == 0);
    Rules p3;
    p3.passes = 3;
    CHECK(p3.max_recycles() == 2 && p1.max_recycles() == 0 && r1.max_recycles() == -1);
    for (int i = 0; i < 2; ++i) {
        CHECK(can_move(w, p3, mv(kWaste, kStock, 2)));
        do_move(w, p3, mv(kWaste, kStock, 2));
        do_move(w, p3, mv(kStock, kWaste, 1));
        do_move(w, p3, mv(kStock, kWaste, 1));
    }
    CHECK(w.recycles == 2 && !can_move(w, p3, mv(kWaste, kStock, 2)));
    CHECK(can_move(w, r1, mv(kWaste, kStock, 2))); /* unlimited */
}

static void test_waste_moves()
{
    Rules r;
    /* waste top AC: any empty foundation */
    Board b = position("- - - -", {"KS", "KH", "", "", "", "", ""}, "QH AC", "", true);
    CHECK(can_move(b, r, mv(kWaste, F(1), 1)) && can_move(b, r, mv(kWaste, F(3), 1)));
    CHECK(!can_move(b, r, mv(kWaste, F(1), 2)));
    CHECK(!can_move(b, r, mv(kWaste, T(3), 1))); /* an Ace on an empty column: King only */
    do_move(b, r, mv(kWaste, F(3), 1));
    CHECK(b.fsuit[2] == Clubs && b.fcount[2] == 1 && b.top(kWaste) == make_card(Hearts, 12));
    /* QH: on KS yes, on KH no (same colour), on an empty column no (King only) or yes (any card) */
    CHECK(can_move(b, r, mv(kWaste, T(1), 1)));
    CHECK(!can_move(b, r, mv(kWaste, T(2), 1)));
    CHECK(!can_move(b, r, mv(kWaste, T(3), 1)));
    Rules any;
    any.fill = TableauFill::AnyCard;
    CHECK(can_move(b, any, mv(kWaste, T(3), 1)));
    CHECK(!can_move(b, r, mv(kWaste, F(3), 1))); /* QH is not the next club */
    /* the next card of a suit goes only to its own foundation */
    Board c = position("C1 - - -", {"KS", "", "", "", "", "", ""}, "2C", "", true);
    CHECK(can_move(c, r, mv(kWaste, F(1), 1)));
    CHECK(!can_move(c, r, mv(kWaste, F(2), 1)));
    Board d = position("C1 - - -", {"KS", "", "", "", "", "", ""}, "3C", "", true);
    CHECK(!can_move(d, r, mv(kWaste, F(1), 1)));
    /* empty waste */
    Board e = position("- - - -", {"KS", "", "", "", "", "", ""}, "", "", true);
    CHECK(!can_move(e, r, mv(kWaste, T(1), 1)) && !can_move(e, r, mv(kWaste, F(1), 1)));
    /* a King from the waste to an empty column */
    Board k = position("- - - -", {"QS", "", "", "", "", "", ""}, "KD", "", true);
    CHECK(can_move(k, r, mv(kWaste, T(2), 1)) && can_move(k, r, mv(kWaste, T(7), 1)));
}

static void test_tableau_moves()
{
    Rules r;
    /* column 1: 8D face down under the run 9S 8H 7C; column 2: TH; column 3: TS; column 4: 9C on 6d; 5 empty; 6: AH;
     * 7: KD QC (a King run at the bottom) */
    Board b = position("- - - -", {"8d 9S 8H 7C", "TH", "TS", "6d 9C", "", "AH", "KD QC"}, "", "", true);
    /* sequence moves */
    CHECK(can_move(b, r, mv(T(1), T(2), 3)));   /* the whole run on a red ten */
    CHECK(!can_move(b, r, mv(T(1), T(3), 3)));  /* not on a black ten */
    CHECK(!can_move(b, r, mv(T(1), T(2), 4)));  /* a face-down card cannot move */
    CHECK(!can_move(b, r, mv(T(1), T(2), 0)));
    CHECK(!can_move(b, r, mv(T(1), T(1), 1)));  /* same column */
    CHECK(can_move(b, r, mv(T(1), T(4), 2)));   /* 8H 7C on 9C */
    CHECK(!can_move(b, r, mv(T(1), T(4), 1)));  /* 7C on 9C: no */
    CHECK(!can_move(b, r, mv(T(1), T(5), 3)));  /* a run from a nine to an empty column: King only */
    Rules any;
    any.fill = TableauFill::AnyCard;
    CHECK(can_move(b, any, mv(T(1), T(5), 3)));
    CHECK(can_move(b, any, mv(T(1), T(5), 1)));
    CHECK(can_move(b, r, mv(T(7), T(5), 2)));   /* the King run to an empty column */
    CHECK(!can_move(b, r, mv(T(7), T(5), 1)));  /* QC alone: King only */
    /* to a foundation: the top card only */
    CHECK(can_move(b, r, mv(T(6), F(2), 1)));
    CHECK(!can_move(b, r, mv(T(6), F(2), 2)));
    CHECK(!can_move(b, r, mv(T(1), F(1), 1)));  /* 7C: clubs have no Ace yet */
    /* the move turns the face-down card over */
    bool flipped = false;
    Board c = b;
    do_move(c, r, mv(T(1), T(2), 3), &flipped);
    CHECK(flipped && c.ntab[0] == 1 && c.ndown[0] == 0 && c.top(T(1)) == make_card(Diamonds, 8));
    CHECK(c.size(T(2)) == 4 && c.top(T(2)) == make_card(Clubs, 7) && c.valid());
    undo_move(c, mv(T(1), T(2), 3), flipped);
    CHECK(c == b);
    do_move(c, r, mv(T(4), T(2), 1), &flipped); /* 9C onto TH turns 6D over */
    CHECK(flipped && c.ndown[3] == 0);
    do_move(c, r, mv(T(6), F(1), 1), &flipped); /* AH up, column 6 empty: nothing to turn */
    CHECK(!flipped && c.ntab[5] == 0 && c.fsuit[0] == Hearts);
    /* invalid kinds */
    CHECK(kind_of(mv(kStock, T(1), 1)) == MoveKind::Invalid && !can_move(b, r, mv(kStock, T(1), 1)));
    CHECK(kind_of(mv(F(1), F(2), 1)) == MoveKind::Invalid && !can_move(b, r, mv(F(1), F(2), 1)));
    CHECK(kind_of(mv(T(1), kWaste, 1)) == MoveKind::Invalid);
}

static void test_foundation_back()
{
    Rules r;
    /* hearts up to 5: 5H may come down on a black six (6S in column 1), not on the red 6D (column 2) */
    Board b = position("H5 - - -", {"6S", "6D", "", "", "", "", ""}, "", "", true);
    CHECK(can_move(b, r, mv(F(1), T(1), 1)));
    CHECK(!can_move(b, r, mv(F(1), T(2), 1)));
    CHECK(!can_move(b, r, mv(F(1), T(3), 1)));  /* not on an empty column (King only) */
    CHECK(!can_move(b, r, mv(F(1), T(1), 2)));
    CHECK(!can_move(b, r, mv(F(2), T(1), 1)));  /* an empty foundation */
    Rules off;
    off.foundation_back = false;
    CHECK(!can_move(b, off, mv(F(1), T(1), 1)));
    do_move(b, r, mv(F(1), T(1), 1));
    CHECK(b.fcount[0] == 4 && b.top(T(1)) == make_card(Hearts, 5));
    /* the last card of a foundation: the pile is empty again and takes any Ace */
    Board a = position("H1 - - -", {"2S", "2D", "", "", "", "", ""}, "", "", true);
    Rules any;
    any.fill = TableauFill::AnyCard;
    CHECK(can_move(a, r, mv(F(1), T(1), 1)));   /* an Ace builds on a two of the other colour */
    CHECK(!can_move(a, r, mv(F(1), T(2), 1)));
    CHECK(!can_move(a, r, mv(F(1), T(3), 1)));  /* not on an empty column (King only) */
    CHECK(can_move(a, any, mv(F(1), T(3), 1))); /* but with fill = any */
    do_move(a, any, mv(F(1), T(3), 1));
    CHECK(a.fcount[0] == 0 && a.fsuit[0] == Board::kNoSuit && a.valid());
}

static void test_legal_moves_and_safe()
{
    Rules r;
    const Board b = deal_board(42);
    std::vector<Move> moves;
    legal_moves(b, r, moves);
    CHECK(!moves.empty() && kind_of(moves[0]) == MoveKind::Draw);
    for (const Move &m : moves) CHECK(can_move(b, r, m));
    /* nothing legal is missing: every combination tried by brute force */
    int legal = 0;
    for (int from = 0; from < kPiles; ++from)
        for (int to = 0; to < kPiles; ++to)
            for (int n = 0; n <= 24; ++n)
                if (can_move(b, r, mv(from, to, n))) ++legal;
    CHECK(legal == static_cast<int>(moves.size()));
    /* safe moves: Aces and twos always; a three only when both black twos are up */
    Board s = position("D2 C1 - -", {"3D", "2C", "", "", "", "", ""}, "", "", true);
    CHECK(safe_to_foundation(s, make_card(Clubs, 2)));
    CHECK(!safe_to_foundation(s, make_card(Diamonds, 3)));
    Move m = safe_auto_move(s);
    CHECK(m.count == 1 && m.from == T(2) && m.to == F(2));
    do_move(s, r, m);
    CHECK(!safe_to_foundation(s, make_card(Diamonds, 3))); /* spades have no two yet */
    CHECK(safe_auto_move(s).count == 0);
    Board s2 = position("D2 C2 S2 -", {"3D", "", "", "", "", "", ""}, "", "", true);
    CHECK(safe_to_foundation(s2, make_card(Diamonds, 3)) && safe_auto_move(s2).from == T(1));
}

/* ------------------------------------------------------------------------------------------------- game */

static void test_undo_redo()
{
    TestRng rng(7);
    for (std::uint32_t deal = 0; deal < 200; ++deal) {
        Rules r;
        r.draw = deal % 2 ? 3 : 1;
        r.passes = deal % 3 == 0 ? 3 : 0;
        r.scoring = deal % 4 == 1 ? Scoring::Vegas : Scoring::Standard;
        r.timed = false;
        Game g;
        g.new_game(deal, r);
        std::vector<Board> boards{g.board()};
        std::vector<std::int32_t> scores{g.score()};
        const int steps = 20 + static_cast<int>(rng.below(150));
        for (int i = 0; i < steps; ++i) {
            const std::vector<Move> moves = g.legal_moves();
            if (moves.empty()) break;
            CHECK(g.apply(moves[rng.below(static_cast<std::uint32_t>(moves.size()))]));
            boards.push_back(g.board());
            scores.push_back(g.score());
        }
        bool ok = true;
        for (std::size_t i = boards.size() - 1; i-- > 0;) {
            ok = ok && g.undo() && g.board() == boards[i] && g.score() == scores[i];
        }
        CHECK(ok && !g.can_undo() && !g.undo());
        for (std::size_t i = 1; i < boards.size(); ++i) ok = ok && g.redo() && g.board() == boards[i] && g.score() == scores[i];
        CHECK(ok && !g.can_redo() && !g.redo());
    }
    /* a new move clears the redo list */
    Game g;
    g.new_game(5, Rules());
    g.apply(g.stock_move());
    g.apply(g.stock_move());
    CHECK(g.undo() && g.can_redo());
    g.apply(g.stock_move());
    CHECK(!g.can_redo());
}

static void test_chained_undo()
{
    /* a move and the safe moves the UI plays after it are one undo step */
    Rules r;
    Game g;
    const Board start = position("- - - -", {"2c AC", "ah 3S", "", "", "", "", ""}, "", "", true);
    CHECK(g.new_game(start, r));
    CHECK(g.apply(mv(T(1), F(1), 1)));         /* AC up turns 2C over */
    int autos = 0;
    for (Move m = g.safe_auto_move(); m.count; m = g.safe_auto_move()) {
        CHECK(g.apply(m, true));
        ++autos;
    }
    CHECK(autos == 1 && g.board().suit_count(Clubs) == 2); /* 2C followed */
    const Board after = g.board();
    CHECK(g.history().size() == 2 && g.history()[1].chained && !g.history()[0].chained);
    CHECK(g.undo() && g.board() == start && !g.can_undo());
    CHECK(g.redo() && g.board() == after && !g.can_redo());
    CHECK(g.moves_made() == 4); /* move, auto move, undo, redo */
    /* a chained flag on the very first step is ignored */
    Game h;
    h.new_game(start, r);
    CHECK(h.apply(mv(T(1), F(1), 1), true) && !h.history()[0].chained);
}

static void test_scoring()
{
    /* the table, one move at a time (Standard, not timed) */
    Rules r;
    r.timed = false;
    Game g;
    CHECK(g.new_game(position("- - - -", {"8d 9S", "TH", "", "", "", "", ""}, "AC 8H", "", true), r));
    CHECK(g.score() == 0);
    CHECK(g.apply(mv(kWaste, T(1), 1)) && g.score() == score::kWasteToTableau);              /* +5 */
    CHECK(g.apply(mv(kWaste, F(1), 1)) && g.score() == 15);                                  /* +10 */
    CHECK(g.apply(mv(T(1), T(2), 2)) && g.score() == 15 + score::kTurnOverTableauCard);      /* +5 turn over */
    CHECK(g.undo() && g.score() == 15);                                                      /* undo gives it back */
    CHECK(g.redo() && g.score() == 20);
    CHECK(g.apply(mv(F(1), T(2), 1)) == false); /* AC does not build on 8H */
    Game h;
    CHECK(h.new_game(position("C2 - - -", {"3D", "", "", "", "", "", ""}, "", "", true), r));
    CHECK(h.apply(mv(F(1), T(1), 1)) && h.score() == 0); /* -15 from 0: never under 0 */
    CHECK(h.undo() && h.score() == 0);
    Game f;
    CHECK(f.new_game(position("C1 - - -", {"3D", "kh 2C", "", "", "", "", ""}, "", "", true), r));
    CHECK(f.apply(mv(T(2), F(1), 1)) && f.score() == score::kTableauToFoundation + score::kTurnOverTableauCard);
    CHECK(f.apply(mv(F(1), T(1), 1)) && f.score() == 15 + score::kFoundationToTableau);      /* 0 */
    CHECK(f.undo() && f.score() == 15);
    /* recycling: draw 1 costs 100 (floored at 0), draw 3 nothing */
    Game d1;
    CHECK(d1.new_game(position("C13 D13 H13 S11", {"", "", "", "", "", "", ""}, "QS KS", "", false), r));
    CHECK(d1.apply(mv(kWaste, kStock, 2)) && d1.score() == 0);
    Game w;
    CHECK(w.new_game(position("C13 D13 H11 S11", {"", "", "", "", "", "", ""}, "QS KS", "KH QH", false), r));
    CHECK(w.apply(mv(kStock, kWaste, 1)) && w.apply(mv(kWaste, F(3), 1)));   /* QH up: +10 */
    CHECK(w.apply(mv(kStock, kWaste, 1)) && w.apply(mv(kWaste, F(3), 1)));   /* KH up: +10 */
    CHECK(w.score() == 20);
    CHECK(w.apply(mv(kWaste, kStock, 2)) && w.score() == 0);                  /* 20 - 100: 0 */
    CHECK(w.undo() && w.score() == 20);
    Rules r3 = r;
    r3.draw = 3;
    Game d3;
    CHECK(d3.new_game(w.board(), r3));
    CHECK(d3.apply(mv(kWaste, kStock, 2)) && d3.score() == 0 && score::kRecycleDrawThree == 0);
    /* the time: -2 every 10 s, and 700000 / seconds when a game of more than 30 s is won */
    CHECK(score::time_penalty(0) == 0 && score::time_penalty(9) == 0 && score::time_penalty(25) == 4);
    CHECK(score::time_bonus(30) == 0 && score::time_bonus(31) == 700000 / 31 && score::time_bonus(100) == 7000);
    Rules timed;
    timed.timed = true;
    Game t;
    CHECK(t.new_game(position("C13 D13 H13 S12", {"KS", "", "", "", "", "", ""}, "", "", false), timed));
    t.add_time(25000);
    CHECK(t.score() == 0); /* nothing to lose yet */
    Game t2;
    CHECK(t2.new_game(position("C13 D13 H12 S12", {"KS", "KH", "", "", "", "", ""}, "", "", false), timed));
    CHECK(t2.apply(mv(T(1), F(4), 1)) && t2.score() == 10);
    t2.add_time(15000);
    CHECK(t2.score() == 8);
    t2.add_time(85000); /* 100 s */
    CHECK(t2.apply(mv(T(2), F(3), 1)) && t2.won());
    CHECK(t2.score() == 20 - 20 + 7000);
    t2.add_time(60000); /* the clock stops at the win */
    CHECK(t2.elapsed_s() == 100 && t2.score() == 7000);
    Game quick;
    CHECK(quick.new_game(position("C13 D13 H13 S12", {"KS", "", "", "", "", "", ""}, "", "", false), timed));
    quick.add_time(20000);
    CHECK(quick.apply(mv(T(1), F(4), 1)) && quick.won() && quick.score() == 10 - 4); /* 30 s or less: no bonus */
    /* Vegas: -52, +5 per foundation card, -5 when one comes back; undo cannot farm */
    Rules vegas;
    vegas.scoring = Scoring::Vegas;
    Game v;
    v.new_game(9, vegas);
    CHECK(v.score() == -52);
    CHECK(v.new_game(position("C2 - - -", {"3D", "", "", "", "", "", ""}, "", "", true), vegas));
    CHECK(v.score() == -52 + 10);
    CHECK(v.apply(mv(F(1), T(1), 1)) && v.score() == -52 + 5);
    CHECK(v.undo() && v.score() == -42 && v.redo() && v.score() == -47);
    Game vw;
    CHECK(vw.new_game(position("C13 D13 H13 S12", {"KS", "", "", "", "", "", ""}, "", "", false), vegas));
    CHECK(vw.apply(mv(T(1), F(4), 1)) && vw.score() == 208);
    /* None */
    Rules none;
    none.scoring = Scoring::None;
    Game n;
    CHECK(n.new_game(position("C1 - - -", {"3D", "kh 2C", "", "", "", "", ""}, "", "", true), none));
    CHECK(n.apply(mv(T(2), F(1), 1)) && n.score() == 0);
}

static void test_game_counters()
{
    Rules r;
    r.passes = 3;
    Game g;
    g.new_game(77, r);
    CHECK(!g.started() && g.moves_made() == 0 && g.recycles_left() == 2 && g.deal_number() == 77);
    CHECK(g.apply(g.stock_move()) && g.moves_made() == 1 && g.started());
    CHECK(!g.apply(mv(kStock, kWaste, 5)) && g.moves_made() == 1); /* refused: nothing changes */
    CHECK(g.undo() && g.moves_made() == 2 && g.redo() && g.moves_made() == 3);
    Rules unl;
    Game u;
    u.new_game(1, unl);
    CHECK(u.recycles_left() == -1);
    g.set_labels(5, true);
    CHECK(g.user_number() == 5 && g.winnable() && g.deal_number() == 77);
    const GameRecord rec = g.record();
    CHECK(!rec.won && rec.moves == 3 && rec.seconds == 0);
    Board bad;
    CHECK(!g.new_game(bad, r)); /* an empty board is not a position */
}

/* ------------------------------------------------------------------------------------------------- package */

static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_text(app + "/root/usr/share/APPLaunch/applications/klondike-solitaire.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(kMinW) + "x" + std::to_string(kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    const std::string portrait = "\nX-Portrait=" + std::to_string(kPortraitW) + "x" + std::to_string(kPortraitH);
    CHECK(desktop.find(portrait) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/klondike-solitaire") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/klondike-solitaire.png") != std::string::npos);
    CHECK(desktop.find("\nX-Background=freeze\n") != std::string::npos);
    CHECK(!read_text(app + "/root/usr/share/APPLaunch/share/images/klondike-solitaire.png").empty());
    CHECK(!read_text(app + "/root/usr/share/doc/klondike-solitaire/copyright").empty());
    const std::string manifest = read_text(app + "/app.json");
    CHECK(manifest.find("\"share_code\": \"klondike-solitaire\"") != std::string::npos);
    CHECK(manifest.find("\"package\": \"klondike-solitaire\"") != std::string::npos);
    CHECK(manifest.find("\"version\": \"0.1.0\"") != std::string::npos);
    CHECK(manifest.find("\"draft\": true") != std::string::npos); /* in development: never published */
    CHECK(manifest.find("\"categories\": [\"Games\"]") != std::string::npos);
    CHECK(manifest.find("\"license\": \"MIT\"") != std::string::npos);
}

int main()
{
    test_cards();
    test_deal();
    test_board_text();
    test_draw_and_recycle();
    test_waste_moves();
    test_tableau_moves();
    test_foundation_back();
    test_legal_moves_and_safe();
    test_undo_redo();
    test_chained_undo();
    test_scoring();
    test_game_counters();
    test_package();
    test_solver_group();
    test_store_group();
    test_layout_group();
    test_session_group();
    test_winsim_group();
    std::printf("test-klondike-solitaire: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
