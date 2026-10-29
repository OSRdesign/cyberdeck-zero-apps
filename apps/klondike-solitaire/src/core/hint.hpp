/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the hint (no LVGL; tests/test_solver.cpp).
 *
 *   hint(game)   the best next move by a ranked heuristic; count 0 when no useful move is left. It never looks at
 *                face-down tableau cards; it does know the order of the stock (as a player who remembers the cards
 *                of earlier passes), so that a "draw" is suggested only when a card the stock can bring up can then
 *                be played, never as endless cycling. Ranks, best first:
 *                  1000  a safe move to a foundation (board.hpp safe_to_foundation)
 *                   950  tableau -> foundation that turns a card over (+ the face-down cards of the column)
 *                   900  tableau -> tableau that turns a card over (+ the face-down cards of the column)
 *                   800  waste -> foundation
 *                   650  waste -> tableau
 *                   600  tableau -> foundation
 *                   550  tableau -> tableau that lets the card under the run go to a foundation
 *                   500  a whole column onto another, when a card is waiting for the empty column (a King, or any
 *                        card with Rules::fill = AnyCard, from the stock or from on top of a face-down card)
 *                   450  draw / recycle toward a stock card that can be played
 *                   300  foundation -> tableau, when a card can then be put on it (from the stock, or a run that
 *                        turns a card over)
 *                Never suggested: the exact reverse of the last move; a run moved between two columns where it
 *                changes nothing (no card turned over, no column emptied for a waiting card, nothing freed); a
 *                whole column moved to an empty one; several empty columns for the same move (only the first).
 *                Following the hints never comes back to an earlier position (tests/test_solver.cpp checks it on
 *                hundreds of deals). Ties keep the order of legal_moves().
 */
#pragma once

#include "game.hpp"

namespace klondike {

Move hint(const Game &game);

} // namespace klondike
