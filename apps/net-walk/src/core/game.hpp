/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: one game (no LVGL, unit tested): the levels, the board in play, the moves counter, the clock.
 *
 * A move is one quarter turn of an unlocked tile that really changes (turning a cross does nothing and is not counted).
 * A locked tile does not turn and is not counted. The clock starts with the first move and stops at the win; the win is
 * the board's state check (Board::is_won), looked at after every move.
 */
#pragma once

#include "board.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace netwalk {

enum class Level : int { Easy = 0, Normal = 1, Hard = 2, Expert = 3 };
constexpr int kLevelCount = 4;

struct LevelInfo {
    const char *key;  /* the key in scores.json */
    const char *name; /* what the screen says */
    const char *dims; /* "5x5" */
    int w, h;
    bool wrap;
};
const LevelInfo &level_info(Level l);
/* the level whose key is `key`, or Normal */
Level level_from_key(const std::string &key, bool *found = nullptr);

class Game {
public:
    Game() { start(Level::Normal, 1); }

    void start(Level level, std::uint64_t seed);
    /* a game on a given board (tests, the UI's fixtures); the level is only a label then */
    void start_with(Level level, std::uint64_t seed, const Board &board);

    Level level() const { return level_; }
    std::uint64_t seed() const { return seed_; }
    const Board &board() const { return board_; }
    Board &board_for_test() { return board_; }
    int moves() const { return moves_; }
    std::uint32_t elapsed_ms() const { return elapsed_ms_; }
    int elapsed_seconds() const { return static_cast<int>(elapsed_ms_ / 1000u); }
    bool started() const { return moves_ > 0; }
    bool won() const { return won_; }
    /* the power of the current state (live: after every move) */
    const std::vector<std::uint8_t> &powered() const { return powered_; }

    /* one quarter turn; true when it counted as a move. False: locked, a cross, or the game is over. */
    bool turn(int i, bool clockwise);
    /* the padlock; false when the game is over (a won board is frozen) */
    bool toggle_lock(int i);
    /* a game in progress with these counters (the UI's test fixtures); the board is looked at again */
    void restore(int moves, std::uint32_t elapsed_ms);
    /* the clock; only runs between the first move and the win */
    void tick(std::uint32_t dt_ms);

private:
    void refresh();

    Level level_ = Level::Normal;
    std::uint64_t seed_ = 1;
    Board board_;
    int moves_ = 0;
    std::uint32_t elapsed_ms_ = 0;
    bool won_ = false;
    std::vector<std::uint8_t> powered_;
};

/* "01:37" (minutes may pass 99: "100:05"); a negative or zero count of none is the caller's "--:--" */
std::string format_time(int seconds);

} // namespace netwalk
