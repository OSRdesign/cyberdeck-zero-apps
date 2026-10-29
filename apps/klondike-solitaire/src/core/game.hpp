/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: one game as the UI plays it (no LVGL, no clock, no threads; tests/test_core.cpp). Board and the
 * move rules are board.hpp, the options and the scoring table rules.hpp.
 *
 *   new_game(number, rules)   deals deal `number` (board.hpp); new_game(board, rules) starts from any valid
 *                             position (tests, the solver's checks). `user_number` / `winnable` are labels the UI
 *                             keeps with the game (a winnable deal shows the user's number, deals another).
 *   apply(move, chained)      plays a legal move (false and nothing changes otherwise). chained = true joins it to
 *                             the previous step for undo / redo: the UI passes it for the safe moves it plays by
 *                             itself after a move, so that one undo takes back the move and its automatic followers.
 *   undo() / redo()           unlimited, a whole group (a move and its chained steps) at a time; a new move clears
 *                             the redo list. The score goes back to what it was before the undone move (rules.hpp).
 *   legal_moves(), stock_move(), safe_auto_move()   what can be played; safe_auto_move() has count 0 when there is
 *                             nothing safe to play (the UI decides whether to play it: Rules::auto_foundation).
 *   won()                     all 52 cards on the foundations.
 *   dead_end(nodes)           no more useful move: nothing can be played from the waste (whatever is drawn), to a
 *                             foundation, or to turn over a tableau card; and either nothing else can be played at
 *                             all (endless stock cycling only), or the solver proves within `nodes` that the game
 *                             cannot be won any more (only back-and-forth moves are left). Cheap unless that solver
 *                             call runs (then at most `nodes`, see solver.hpp).
 *   add_time(ms)              the UI adds the time that passed while the game was in front (paused when hidden or in
 *                             a menu, as it chooses); nothing is added once the game is won.
 *   score()                   what to show now: Standard = points - time penalty (timed), + the time bonus once
 *                             won; Vegas = -52 + 5 per foundation card; None = 0 (rules.hpp).
 *   moves_made()              every action counts one: a move (an automatic one too), a draw, a recycle, an undo, a
 *                             redo.
 *   record()                  what Stats::record() needs when the game ends (won, or given up after a move).
 *   save() / load()           the game as JSON (store.hpp writes it): rules, deal number, start position, history
 *                             and redo list, time and moves counter. load() replays the history move by move and
 *                             refuses anything that does not replay exactly (false, `why` says why).
 */
#pragma once

#include "board.hpp"
#include "rules.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace klondike {

struct Step {
    Move move;
    bool flipped = false;      /* a tableau card was turned over after it */
    bool chained = false;      /* undone and redone with the step before it */
    std::int32_t score_before = 0;
};

struct GameRecord {
    bool won = false;
    std::int32_t score = 0;
    std::uint32_t seconds = 0;
    std::uint32_t moves = 0;
};

constexpr std::uint32_t kDeadEndNodes = 20000;

class Game {
public:
    Game();
    void new_game(std::uint32_t number, const Rules &rules);
    bool new_game(const Board &start, const Rules &rules, std::uint32_t number = 0);
    void set_labels(std::uint32_t user_number, bool winnable);

    const Board &board() const { return board_; }
    const Board &start() const { return start_; }
    const Rules &rules() const { return rules_; }
    std::uint32_t deal_number() const { return number_; }
    std::uint32_t user_number() const { return user_number_; }
    bool winnable() const { return winnable_; }

    bool can_apply(const Move &m) const { return can_move(board_, rules_, m); }
    bool apply(const Move &m, bool chained = false);
    bool can_undo() const { return !history_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    const std::vector<Step> &history() const { return history_; }
    const std::vector<Step> &redo_list() const { return redo_; }

    std::vector<Move> legal_moves() const;
    Move stock_move() const { return klondike::stock_move(board_, rules_); }
    Move safe_auto_move() const { return klondike::safe_auto_move(board_); }
    bool won() const { return board_.won(); }
    bool dead_end(std::uint32_t nodes = kDeadEndNodes) const;
    /* a move was made (an untouched deal is not worth a statistics entry) */
    bool started() const { return moves_made_ > 0; }
    /* recycles still allowed: -1 = unlimited */
    int recycles_left() const;

    void add_time(std::uint32_t ms);
    std::uint64_t elapsed_ms() const { return elapsed_ms_; }
    std::uint32_t elapsed_s() const { return static_cast<std::uint32_t>(elapsed_ms_ / 1000); }
    std::int32_t score() const;
    std::uint32_t moves_made() const { return moves_made_; }
    GameRecord record() const;

    std::string save() const;
    static bool load(const std::string &json, Game &out, std::string *why = nullptr);

private:
    void play(const Move &m, bool chained);
    std::int32_t points_after(const Move &m, bool flipped, std::int32_t before) const;

    Board start_;
    Board board_;
    Rules rules_;
    std::uint32_t number_ = 0;
    std::uint32_t user_number_ = 0;
    bool winnable_ = false;
    std::vector<Step> history_;
    std::vector<Step> redo_;
    std::int32_t points_ = 0;          /* Standard running points (>= 0), before time penalty and bonus */
    std::uint64_t elapsed_ms_ = 0;
    std::uint32_t moves_made_ = 0;
};

} // namespace klondike
