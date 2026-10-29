/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the statistics (no LVGL; tests/test_store.cpp). store.hpp keeps them in stats.json.
 *
 *   One VariationStats per Rules::variation_key() (draw, passes, scoring: "d1-unl-standard", "d3-p3-vegas", ...).
 *   record(rules, game.record()) when a game ends: won, or given up / replaced by a new deal after at least one move
 *   (Game::started(); the UI decides). It counts the game played (and won), adds its time, keeps the streaks (a win
 *   adds one to the current streak, anything else sets it to 0), the best score (all recorded games; not kept with
 *   scoring None), the best time and the fewest moves (won games only), and for Vegas adds the game's score
 *   (-52 + 5 per foundation card) to the variation's bank, which can go below zero and carries over for ever (until
 *   reset). The bank while a Vegas game is being played = bank + Game::score().
 *
 *   JSON (stats.json): {"version":1,"variations":{"d1-unl-standard":{"played":3,"won":1,"best_score":120,
 *   "best_time":95,"fewest_moves":130,"streak":0,"best_streak":1,"total_time":800,"bank":0}, ...}}; best_score is
 *   left out while there is none, best_time / fewest_moves are 0 while there is none.
 */
#pragma once

#include "game.hpp"
#include "rules.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace klondike {

struct VariationStats {
    std::uint32_t played = 0;
    std::uint32_t won = 0;
    bool has_best_score = false;
    std::int32_t best_score = 0;
    std::uint32_t best_time_s = 0;      /* 0 = no win yet */
    std::uint32_t fewest_moves = 0;     /* 0 = no win yet */
    std::uint32_t streak = 0;
    std::uint32_t best_streak = 0;
    std::uint64_t total_time_s = 0;
    std::int64_t vegas_bank = 0;

    bool operator==(const VariationStats &o) const;
};

class Stats {
public:
    void record(const Rules &rules, const GameRecord &game);
    /* zeros for a variation never played */
    VariationStats get(const std::string &key) const;
    const std::map<std::string, VariationStats> &all() const { return by_key_; }
    void reset(const std::string &key) { by_key_.erase(key); }
    void reset_all() { by_key_.clear(); }

    std::string to_json() const;
    static bool from_json(const std::string &text, Stats &out, std::string *why = nullptr);
    bool operator==(const Stats &o) const { return by_key_ == o.by_key_; }

private:
    std::map<std::string, VariationStats> by_key_;
};

} // namespace klondike
