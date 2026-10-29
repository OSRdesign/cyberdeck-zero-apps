/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the rule options and THE scoring table (no LVGL; tests/test_core.cpp).
 *
 *   Rules           the options of one game, serialisable as one line of "key=value" pairs:
 *                     "draw=1,passes=0,scoring=standard,timed=1,auto=1,fback=1,fill=king"
 *                   draw 1 | 3; passes 0 (unlimited) | 3 | 1 (passes through the stock: 3 passes = 2 recycles);
 *                   scoring standard | vegas | none; timed 0 | 1 (Standard only: time penalty and bonus);
 *                   auto 0 | 1 (the UI plays the safe moves to the foundations by itself; the engine only answers
 *                   Game::safe_auto_move()); fback 0 | 1 (a foundation card may come back to the tableau);
 *                   fill king | any (what may go on an empty tableau column). parse() ignores unknown keys and keeps
 *                   the default of a missing or bad one, so an older or newer settings line still loads.
 *   variation_key   the statistics key: draw, passes and scoring only, e.g. "d1-unl-standard", "d3-p3-vegas".
 *
 * Scoring (namespace score), the one place where the points live:
 *
 *   Standard                                   points   notes
 *   waste -> tableau                              +5
 *   waste -> foundation                          +10
 *   tableau -> foundation                        +10
 *   turn over a tableau card                      +5    the engine turns it over by itself after the move
 *   foundation -> tableau                        -15
 *   recycle the waste, draw 1                   -100    the score never goes under 0 (Windows rule)
 *   recycle the waste, draw 3                      0
 *   tableau -> tableau, draw                       0
 *   timed games: every 10 s of play               -2    shown score = max(0, points - 2 * (seconds / 10))
 *   timed games: bonus when won                700000 / seconds, only when the game took more than 30 s
 *   undo                                  restores the score from before the undone move (so turning a card over
 *                                         and undoing it gives nothing back; undoing a -15 gives the 15 back)
 *
 *   Vegas: the game costs 52 at the deal and pays 5 per card on the foundations, so its score is always
 *   -52 + 5 * (cards on the foundations): undo, a card taken back from a foundation (-5) or replayed cannot farm
 *   points. The bank (Stats) adds each finished game's score; it carries over from game to game per variation.
 *
 *   None: always 0.
 */
#pragma once

#include <cstdint>
#include <string>

namespace klondike {

/* The smallest screen the app supports: X-MinSize of its .desktop file (tests/test_core.cpp compares them). */
constexpr int kMinW = 640;
constexpr int kMinH = 480;
/* The smallest portrait canvas (X-Portrait of the .desktop file; the UI is laid out for any taller-than-wide canvas). */
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 640;

enum class Scoring : std::uint8_t { Standard, Vegas, None };
enum class TableauFill : std::uint8_t { KingOnly, AnyCard };

struct Rules {
    int draw = 1;                         /* 1 or 3 */
    int passes = 0;                       /* 0 = unlimited, 3 or 1 */
    Scoring scoring = Scoring::Standard;
    bool timed = true;
    bool auto_foundation = true;
    bool foundation_back = true;
    TableauFill fill = TableauFill::KingOnly;

    /* Recycles allowed in one game: -1 = unlimited, else passes - 1. */
    int max_recycles() const { return passes == 0 ? -1 : passes - 1; }
    std::string serialize() const;
    /* Fills `out` from `text` (starting from the defaults); false when nothing in it was understood. */
    static bool parse(const std::string &text, Rules &out);
    std::string variation_key() const;

    bool operator==(const Rules &o) const;
    bool operator!=(const Rules &o) const { return !(*this == o); }
};

namespace score {
constexpr int kWasteToTableau = 5;
constexpr int kWasteToFoundation = 10;
constexpr int kTableauToFoundation = 10;
constexpr int kTurnOverTableauCard = 5;
constexpr int kFoundationToTableau = -15;
constexpr int kRecycleDrawOne = -100;
constexpr int kRecycleDrawThree = 0;
constexpr int kTimePenalty = -2;            /* per kTimePenaltySeconds of play, timed Standard games */
constexpr int kTimePenaltySeconds = 10;
constexpr std::int64_t kTimeBonus = 700000; /* divided by the seconds of a won timed Standard game */
constexpr int kTimeBonusMinSeconds = 30;    /* no bonus for a game of 30 s or less */
constexpr int kVegasStart = -52;
constexpr int kVegasPerFoundationCard = 5;

/* Standard: points taken off a running score after `seconds` of a timed game (>= 0). */
std::int64_t time_penalty(std::uint32_t seconds);
/* Standard: bonus of a won timed game that took `seconds`. */
std::int64_t time_bonus(std::uint32_t seconds);
/* Vegas: the game's score with `foundation_cards` on the foundations. */
constexpr int vegas(int foundation_cards) { return kVegasStart + kVegasPerFoundationCard * foundation_cards; }
} // namespace score

} // namespace klondike
