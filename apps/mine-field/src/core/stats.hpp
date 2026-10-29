/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the statistics (no LVGL; tests/test_store.cpp). store.hpp keeps them in stats.json.
 *
 *   One PresetStats for each of Beginner, Intermediate and Expert (a board's size comes from the screen; each best entry keeps
 *   its own w x h and mines for display).
 *   record(game.record()) when a game ends: won, or given up / replaced by a new game after the first reveal
 *   (GameRecord::started; the Session decides, as Klondike's rules do: an abandoned game counts as a loss; a game never
 *   started is not recorded at all). It counts the game played (and won), adds its time (clock + hint penalty) to the total,
 *   keeps the streaks (a win adds one to the current streak, a loss sets it to 0) and, for a win without hints
 *   (not assisted), puts the time in the best five: ascending by time, an equal time ranks after the older one, five kept.
 *   A won game that used hints counts as a win (and in the streak) but is never in the best times. Fair-board games share
 *   the table; each entry carries its `fair` flag and its seed.
 *
 *   JSON (stats.json): {"version":1,"presets":{"beginner":{"played":3,"won":1,"streak":0,"best_streak":1,
 *   "total_time_ms":800000,"best":[{"time_ms":95000,"seed":"123","fair":true}]}, ...}}; seeds are decimal strings
 *   (64 bit).
 */
#pragma once

#include "game.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace minefield {

constexpr int kBestCount = 5;

struct BestEntry {
    std::uint32_t time_ms = 0;
    std::uint64_t seed = 0;
    bool fair = false;
    int w = 0, h = 0, mines = 0;          /* the board it was won on (the screen decides it, so it is kept for display) */
    bool operator==(const BestEntry &o) const
    {
        return time_ms == o.time_ms && seed == o.seed && fair == o.fair && w == o.w && h == o.h && mines == o.mines;
    }
};

struct PresetStats {
    std::uint32_t played = 0;
    std::uint32_t won = 0;
    std::uint32_t streak = 0;
    std::uint32_t best_streak = 0;
    std::uint64_t total_time_ms = 0;
    std::vector<BestEntry> best;          /* ascending, at most kBestCount */

    bool operator==(const PresetStats &o) const;
};

class Stats {
public:
    /* false when the game is not recorded (never started). */
    bool record(const GameRecord &game);
    /* zeros for a preset never played */
    PresetStats get(Preset p) const;
    /* Where `time_ms` would rank among the best times of the preset (0 = best), -1 when it would not make the five. */
    int rank_of(Preset p, std::uint32_t time_ms) const;
    void reset(Preset p);
    void reset_all();

    std::string to_json() const;
    static bool from_json(const std::string &text, Stats &out, std::string *why = nullptr);
    bool operator==(const Stats &o) const { return by_.size() == o.by_.size() && same(o); }

private:
    bool same(const Stats &o) const;
    std::array<PresetStats, kRecordedPresets> by_{};
};

} // namespace minefield
