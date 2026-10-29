/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike UI: what the screen needs besides the board, with no LVGL (tests/test_session.cpp): the rules for the next
 * game, the look preferences, the statistics, the saved game, and the rules about when a game counts.
 *
 *   UiPrefs      the look and feel: left-handed, four-colour deck, felt (0..2), card back (0..1), animation
 *                (0 on, 1 reduced, 2 off) and "winnable deals". Kept in <config>/ui.json (atomic like the rules file).
 *   Session      owns the Game, the Stats, the next-game Rules, the UiPrefs and the Store.
 *     load()           stats, rules and prefs, then the game in progress (resumed: returns true) or a fresh random deal.
 *                      A file that cannot be read is started afresh (Store sets it aside as .bad).
 *     begin_deal()     a new game on `deal` with the NEXT rules; the game it replaces counts as a loss when a move was
 *                      made and not all taken back (history not empty) and it was not won; a win was recorded at once.
 *     restart()        the same start position again (counts like a new game for the one it replaces), with the rules
 *                      the game was dealt with.
 *     play()           a legal move; a win (all 52 on the foundations) is recorded in the statistics and the saved game
 *                      removed. The game is saved after every change (save_game(), cheap).
 *     auto_moves()     after a user move: the safe moves to the foundations, chained to it for undo (only with
 *                      Rules::auto_foundation); returns how many.
 *     undo()/redo()    refused once the game is won (its result is in the statistics).
 *     tick(ms)         the clock: time passes only while the UI says so (in front, not in a menu), and only after the
 *                      first move; refused once won.
 *   The UI never changes the rules of a game in progress: set_rules() changes the NEXT game's rules only.
 */
#pragma once

#include "game.hpp"
#include "stats.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace klondike {

struct UiPrefs {
    bool left_handed = false;
    bool four_colour = false;
    int felt = 0;
    int back = 0;
    int anim = 0;          /* 0 on, 1 reduced, 2 off */
    bool winnable = false; /* "winnable deals" for the next random game */
    bool operator==(const UiPrefs &o) const
    {
        return left_handed == o.left_handed && four_colour == o.four_colour && felt == o.felt && back == o.back &&
               anim == o.anim && winnable == o.winnable;
    }
};

std::string prefs_to_json(const UiPrefs &p);
bool prefs_from_json(const std::string &text, UiPrefs &out);

class Session {
public:
    explicit Session(Store store);

    bool load();

    Game &game() { return game_; }
    const Game &game() const { return game_; }
    const Rules &rules() const { return rules_; }
    void set_rules(const Rules &r);
    UiPrefs &prefs() { return prefs_; }
    const UiPrefs &prefs() const { return prefs_; }
    void save_prefs();
    const Stats &stats() const { return stats_; }
    void reset_stats(const std::string &key);
    void reset_all_stats();
    Store &store() { return store_; }

    /* the game has a move that is not taken back (an untouched deal costs nothing) */
    bool in_progress() const { return !recorded_ && !game_.history().empty(); }
    bool finished() const { return recorded_; }
    /* the rules of the next game differ from the ones of this game */
    bool rules_pending() const { return rules_ != game_.rules(); }

    std::uint32_t random_deal();
    void begin_deal(std::uint32_t deal, std::uint32_t user, bool winnable);
    void restart();

    bool play(const Move &m, bool chained = false);
    int auto_moves();
    bool undo();
    bool redo();
    void tick(std::uint32_t ms);
    void save_game();

    /* Vegas: the bank of the variation of this game while it is played (bank + score), else 0 */
    std::int64_t vegas_bank_now() const;

private:
    void abandon_current();
    void finish_if_won();

    Store store_;
    Game game_;
    Stats stats_;
    Rules rules_;
    UiPrefs prefs_;
    bool recorded_ = false;
    std::uint64_t rng_ = 0;
};

/* The command lines of the solver helper process (ui/worker.cpp runs them; pure helpers so that the parent and the child
 * agree and the tests can check them). */
std::string worker_winnable_reply(std::uint32_t user, const Rules &rules);
std::string worker_deadend_reply(const std::string &board_text, const Rules &rules);
bool parse_worker_winnable(const std::string &reply, bool &found, std::uint32_t &deal);
bool parse_worker_deadend(const std::string &reply, bool &dead);

} // namespace klondike
