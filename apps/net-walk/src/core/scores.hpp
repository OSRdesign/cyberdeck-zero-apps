/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the best moves and the best time of each level, kept as JSON in the app's data folder
 * ($NETWALK_DATA, else $XDG_DATA_HOME/net-walk, else ~/.local/share/net-walk), file scores.json:
 *
 *   {"version":1,"last":"normal","best":{"easy":{"moves":12,"seconds":31},"normal":{"moves":0,"seconds":0}, ...}}
 *
 * 0 means "no score yet". The two records are independent (the fewest moves and the shortest time may come from
 * different games). A damaged file is read as no scores (and left alone until the next save replaces it).
 */
#pragma once

#include "game.hpp"

#include <string>
#include <utility>

namespace netwalk {

struct Best {
    int moves = 0;
    int seconds = 0;
};

struct Record {
    bool new_moves = false;
    bool new_time = false;
};

class Scores {
public:
    const Best &best(Level l) const { return best_[static_cast<int>(l)]; }
    /* a finished game; the seconds are at least 1 so that 0 stays "none" */
    Record record(Level l, int moves, int seconds);
    void clear();
    /* the level of the last game, so the app opens on it again */
    Level last() const { return last_; }
    void set_last(Level l) { last_ = l; }

    std::string to_json() const;
    /* false for text that is not a scores file (the book is then empty) */
    static bool from_json(const std::string &text, Scores &out);

private:
    Best best_[kLevelCount];
    Level last_ = Level::Normal;
};

class ScoreStore {
public:
    ScoreStore();
    explicit ScoreStore(std::string dir) : dir_(std::move(dir)) {}
    static std::string default_dir();
    const std::string &dir() const { return dir_; }
    Scores load() const;
    bool save(const Scores &s) const;

private:
    std::string dir_;
};

} // namespace netwalk
