/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the hint (see hint.hpp).
 */
#include "hint.hpp"

namespace klondike {

namespace {

/* a card can be played somewhere from the waste top */
bool playable_from_waste(const Board &b, const Rules &r, Card c)
{
    if (foundation_for(b, c) >= 0) return true;
    for (int t = 0; t < kColumns; ++t)
        if (fits_column(b, r, c, t)) return true;
    return false;
}

} // namespace

Move hint(const Game &game)
{
    const Board &b = game.board();
    const Rules &r = game.rules();
    const bool has_last = !game.history().empty();
    const Move last = has_last ? game.history().back().move : Move();
    const MoveKind last_kind = kind_of(last);
    auto is_reverse = [&](const Move &m) {
        return has_last && last_kind != MoveKind::Draw && last_kind != MoveKind::Recycle && m.from == last.to &&
               m.to == last.from && m.count == last.count;
    };

    int first_empty = -1;
    for (int c = 0; c < kColumns; ++c)
        if (b.ntab[c] == 0) {
            first_empty = c;
            break;
        }

    /* cards the stock can bring to the waste top (any number of draws) */
    std::vector<Card> talon_cards;
    bool playable_ahead = false;
    for_each_reachable_talon_card(b, r, [&](Card c, int ops, int) {
        talon_cards.push_back(c);
        if (ops > 0 && playable_from_waste(b, r, c)) playable_ahead = true;
    });

    /* a card waiting for an empty column: from the stock, or a run base lying on a face-down card */
    auto fills_empty = [&](Card c) { return rank_of(c) == kRanks || r.fill == TableauFill::AnyCard; };
    bool filler_waiting = false;
    for (Card c : talon_cards)
        if (fills_empty(c)) filler_waiting = true;
    for (int s = 0; s < kColumns && !filler_waiting; ++s)
        if (b.ntab[s] > b.ndown[s] && b.ndown[s] > 0 && fills_empty(b.tab[s][b.ndown[s]])) filler_waiting = true;

    /* something useful could go on card c once it is in column `col` (foundation -> tableau) */
    auto useful_target = [&](Card c, int col) {
        for (Card t : talon_cards)
            if (builds_on(t, c)) return true;
        for (int s = 0; s < kColumns; ++s) {
            if (s == col || b.ntab[s] == 0 || b.ndown[s] == 0 || b.ndown[s] >= b.ntab[s]) continue;
            if (builds_on(b.tab[s][b.ndown[s]], c)) return true; /* that run would turn a card over */
        }
        return false;
    };

    std::vector<Move> moves;
    legal_moves(b, r, moves);
    Move best;
    int best_score = 0;
    for (const Move &m : moves) {
        const MoveKind k = kind_of(m);
        if (k == MoveKind::Draw || k == MoveKind::Recycle || is_reverse(m)) continue;
        if (is_tableau(m.to) && b.ntab[m.to - kTableau0] == 0 && m.to - kTableau0 != first_empty) continue;
        int score = 0;
        switch (k) {
        case MoveKind::WasteToFoundation:
            score = safe_to_foundation(b, b.talon[b.tp - 1]) ? 1000 : 800;
            break;
        case MoveKind::WasteToTableau: score = 650; break;
        case MoveKind::TableauToFoundation: {
            const int s = m.from - kTableau0;
            const bool exposes = b.ntab[s] - 1 == b.ndown[s] && b.ndown[s] > 0;
            if (safe_to_foundation(b, b.tab[s][b.ntab[s] - 1])) score = 1000 + (exposes ? 10 + b.ndown[s] : 0);
            else score = exposes ? 950 + b.ndown[s] : 600;
            break;
        }
        case MoveKind::TableauToTableau: {
            const int s = m.from - kTableau0;
            const int base = b.ntab[s] - m.count;
            const bool to_empty = b.ntab[m.to - kTableau0] == 0;
            if (base == b.ndown[s] && base > 0) score = 900 + b.ndown[s];
            else if (base == 0) score = (!to_empty && filler_waiting) ? 500 : 0;
            else score = foundation_for(b, b.tab[s][base - 1]) >= 0 ? 550 : 0;
            break;
        }
        case MoveKind::FoundationToTableau: {
            const int f = m.from - kFoundation0;
            const Card c = make_card(b.fsuit[f], b.fcount[f]);
            score = useful_target(c, m.to - kTableau0) ? 300 : 0;
            break;
        }
        default: break;
        }
        if (score > best_score) {
            best_score = score;
            best = m;
        }
    }
    if (best_score < 450 && playable_ahead) {
        const Move sm = stock_move(b, r);
        if (sm.count) best = sm;
    }
    return best;
}

} // namespace klondike
