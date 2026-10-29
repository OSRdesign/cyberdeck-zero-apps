/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the board, the moves and their rules, the deal (no LVGL; tests/test_core.cpp). Game (game.hpp)
 * adds history, score and time on top; the solver (solver.hpp) works on Board directly.
 *
 * Piles, by id (Move::from / Move::to):  0 stock, 1 waste, 2..5 foundations, 6..12 tableau columns (left to right).
 *   size(p), card(p, i) (i = 0 is the bottom, size - 1 the top), face_up(p, i), top(p).
 *   A foundation takes any Ace when empty and then keeps that suit; fsuit[] / fcount[] say which and how far.
 *   The stock and the waste are one sequence `talon` in draw order with a pointer `tp`: the waste is talon[0, tp)
 *   (its top is talon[tp - 1]), the stock is talon[tp, ntalon) (its top, the next card drawn, is talon[tp]). Drawing
 *   only moves tp, a recycle sets it back to 0, playing the waste top removes talon[tp - 1].
 *
 * Moves (Move = from, to, count), all checked by can_move():
 *   draw          0 -> 1, count = min(draw, stock size)
 *   recycle       1 -> 0, count = waste size; only with an empty stock and while recycles < Rules::max_recycles()
 *   waste         1 -> foundation | tableau, count 1
 *   tableau       tableau -> foundation (count 1, the top card) | tableau (count = 1 .. face-up cards: the moved run
 *                 is the top `count` cards; its bottom card must build on the target's top, or the target is empty
 *                 and the card is a King, or any card with Rules::fill = AnyCard)
 *   foundation    foundation -> tableau, count 1, only with Rules::foundation_back
 *   After a move from a tableau column whose new top is face down, do_move() turns it over (`flipped`).
 *
 * The deal (deal_board): a 52-card deck in id order shuffled by Fisher-Yates driven by SplitMix64, then dealt as
 * by hand: row r = 0..6 gives one card to each column c = r..6, the last card of each column face up; the 24 cards
 * left are the stock, the next card of the deck on top. The generator, so that a deal number gives the same deal on
 * every board and in every version:
 *   state = number * 0x9E3779B97F4A7C15 ^ 0x4B4C4F4E44494B45 (64-bit);
 *   next():  state += 0x9E3779B97F4A7C15; z = state; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
 *            z = (z ^ (z >> 27)) * 0x94D049BB133111EB; return z ^ (z >> 31);
 *   below(n): draws next() again while it is >= 2^64 - (2^64 mod n) (no modulo bias), then returns it mod n;
 *   for i = 51 down to 1: swap deck[i] with deck[below(i + 1)]; deck[0] is dealt first.
 * layout_checksum() is FNV-1a (32 bit) over the dealt order (columns left to right bottom to top, then the stock from
 * its top) and pins the generator in the tests.
 *
 * Board text (to_text / from_text, used by the saved game and the tests), lines separated by '\n' or ';':
 *   f C1 D13 H4 -          the four foundations in pile order: suit letter and count, '-' when empty
 *   t 2c 3C                a tableau column, bottom first; lower case = face down; one 't' line per column (7)
 *   w 5H 9S                the waste, bottom first
 *   s QH JC                the stock, bottom first (the last one is drawn next)
 *   r 0                    recycles used
 * The 52 cards must all be there once (the foundations hold suit ranks 1..count).
 */
#pragma once

#include "card.hpp"
#include "rules.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace klondike {

enum PileId : std::uint8_t { kStock = 0, kWaste = 1, kFoundation0 = 2, kTableau0 = 6, kPiles = 13 };
constexpr int kFoundations = 4;
constexpr int kColumns = 7;
constexpr int kMaxColumn = 19; /* 6 face down + King to Ace */

constexpr int foundation_pile(int f) { return kFoundation0 + f; }
constexpr int tableau_pile(int c) { return kTableau0 + c; }
constexpr bool is_foundation(int pile) { return pile >= kFoundation0 && pile < kTableau0; }
constexpr bool is_tableau(int pile) { return pile >= kTableau0 && pile < kPiles; }

struct Move {
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint8_t count = 0;
    bool operator==(const Move &o) const { return from == o.from && to == o.to && count == o.count; }
    bool operator!=(const Move &o) const { return !(*this == o); }
};

enum class MoveKind {
    Draw, Recycle, WasteToFoundation, WasteToTableau, TableauToFoundation, TableauToTableau, FoundationToTableau,
    Invalid
};
MoveKind kind_of(const Move &m);
/* "draw", "recycle", "W>F1", "T3>T5x2" (piles counted from 1), for logs and tests. */
std::string move_name(const Move &m);
/* "from-to-count", the saved-game form; parse_move() reads it back (false on anything else). */
std::string encode_move(const Move &m);
bool parse_move(const std::string &text, Move &out);

struct Board {
    Card talon[kCards];
    std::uint8_t ntalon = 0;
    std::uint8_t tp = 0;
    std::uint8_t fsuit[kFoundations];          /* kNoSuit while empty */
    std::uint8_t fcount[kFoundations];
    Card tab[kColumns][kMaxColumn];
    std::uint8_t ntab[kColumns];
    std::uint8_t ndown[kColumns];
    std::uint8_t recycles = 0;

    static constexpr std::uint8_t kNoSuit = 0xFF;

    Board();
    int size(int pile) const;
    Card card(int pile, int i) const;
    bool face_up(int pile, int i) const;
    Card top(int pile) const;
    int stock_size() const { return ntalon - tp; }
    int waste_size() const { return tp; }
    /* Cards of `suit` on the foundations (0..13), whichever pile holds them. */
    int suit_count(int suit) const;
    int foundation_cards() const;
    bool won() const { return foundation_cards() == kCards; }
    /* The 52 cards once each, foundations in order, columns with face-down cards under face-up ones, every face-up
     * run built down in alternating colours. `why` says what is wrong. */
    bool valid(std::string *why = nullptr) const;
    /* Exact position hash (64 bit): every pile, the stock pointer and the recycles. */
    std::uint64_t hash() const;
    std::string to_text() const;
    static bool from_text(const std::string &text, Board &out, std::string *why = nullptr);
    bool operator==(const Board &o) const;
    bool operator!=(const Board &o) const { return !(*this == o); }
};

Board deal_board(std::uint32_t number);
std::uint32_t layout_checksum(const Board &dealt);

/* ------------------------------------------------------------------------------------------------- the rules */

bool can_move(const Board &b, const Rules &r, const Move &m);
/* Plays a move that can_move() accepted. *flipped (may be null) = a tableau card was turned over after it. */
void do_move(Board &b, const Rules &r, const Move &m, bool *flipped = nullptr);
/* Takes back a move do_move() played on this position, with the flipped flag it returned. */
void undo_move(Board &b, const Move &m, bool flipped);
/* Every legal move, in pile order: draw or recycle first, then from the waste, the tableau, the foundations (an Ace
 * is listed once per empty foundation, a King once per empty column: every place it may be dropped). */
void legal_moves(const Board &b, const Rules &r, std::vector<Move> &out);
/* The move a tap on the stock makes: draw, or recycle; count 0 when neither is possible. */
Move stock_move(const Board &b, const Rules &r);
/* The foundation pile (2..5) that takes card c now, or -1. */
int foundation_for(const Board &b, Card c);
/* Card c (the bottom of a moved run) may go on tableau column `column` (0..6) now. */
bool fits_column(const Board &b, const Rules &r, Card c, int column);
/* c can go to a foundation and no card could ever need it as a target any more: its rank is 1 or 2, or both
 * foundations of the other colour hold its rank - 1 already. Moving such a card up never loses a game. */
bool safe_to_foundation(const Board &b, Card c);
/* The first safe move to a foundation (tableau tops left to right, then the waste top); count 0 when none. */
Move safe_auto_move(const Board &b);

/* The cards the waste top can become by drawing and recycling, each position once and by the fewest operations:
 * visit(card, ops, position) with ops = draws and recycles needed (0 = the current waste top) and position = its
 * index in talon[]. At most one recycle, none beyond the rule's limit. */
template <class Visit> void for_each_reachable_talon_card(const Board &b, const Rules &r, Visit &&visit);

/* ------------------------------------------------------------------------------------------------- inline */

template <class Visit> void for_each_reachable_talon_card(const Board &b, const Rules &r, Visit &&visit)
{
    const int n = b.ntalon;
    if (n == 0) return;
    const int draw = r.draw < 1 ? 1 : r.draw;
    const int max_rec = r.max_recycles();
    const int start_tp = b.tp;
    int tp = b.tp;
    int ops = 0;
    bool recycled = false;
    /* positions are visited in the order they become the waste top; after the recycle only those the first pass
     * did not show (with draw 3 the second pass is aligned differently) */
    std::uint64_t seen = 0;
    if (tp > 0) {
        seen |= std::uint64_t(1) << (tp - 1);
        visit(b.talon[tp - 1], 0, tp - 1);
    }
    for (;;) {
        if (tp < n) {
            tp += (n - tp < draw) ? n - tp : draw;
            ++ops;
            const int pos = tp - 1;
            if (seen & (std::uint64_t(1) << pos)) continue;
            seen |= std::uint64_t(1) << pos;
            visit(b.talon[pos], ops, pos);
        } else {
            if (recycled || start_tp == 0) break; /* a second pass would show the same cards again */
            if (max_rec >= 0 && b.recycles >= max_rec) break;
            tp = 0;
            ++ops;
            recycled = true;
        }
    }
}

} // namespace klondike
