/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the cards (no LVGL; tests/test_core.cpp).
 *
 *   Card          one byte, id = suit * 13 + (rank - 1), 0..51; kNoCard (0xFF) = none.
 *   Suits         in this order: Clubs 0, Diamonds 1, Hearts 2, Spades 3 (Diamonds and Hearts are red).
 *   Ranks         1 (Ace) .. 13 (King).
 *   builds_on     the tableau rule: b may lie on a when the colours differ and b is one rank lower.
 *   card_name     two characters, rank then suit: "AS", "TH" (ten of hearts), "QC", "7D".
 *   parse_card    reads that form (also "10H"); lower case ("as", "th") means face down, which only the board text
 *                 format (board.hpp) uses.
 */
#pragma once

#include <cstdint>
#include <string>

namespace klondike {

using Card = std::uint8_t;
constexpr Card kNoCard = 0xFF;
constexpr int kCards = 52;
constexpr int kRanks = 13;
constexpr int kSuits = 4;

enum Suit : std::uint8_t { Clubs = 0, Diamonds = 1, Hearts = 2, Spades = 3 };

constexpr Card make_card(int suit, int rank) { return static_cast<Card>(suit * kRanks + rank - 1); }
constexpr int suit_of(Card c) { return c / kRanks; }
constexpr int rank_of(Card c) { return c % kRanks + 1; }
constexpr bool is_red(Card c) { return suit_of(c) == Diamonds || suit_of(c) == Hearts; }
constexpr bool is_red_suit(int suit) { return suit == Diamonds || suit == Hearts; }
constexpr bool builds_on(Card b, Card a) { return is_red(a) != is_red(b) && rank_of(a) == rank_of(b) + 1; }

std::string card_name(Card c);
bool parse_card(const std::string &text, Card &out, bool *face_down = nullptr);

} // namespace klondike
