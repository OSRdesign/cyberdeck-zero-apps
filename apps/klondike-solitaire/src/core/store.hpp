/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: where the app keeps its files (no LVGL; tests/test_store.cpp). Two folders (injectable for the
 * tests, created on the first save):
 *
 *   data    $KLONDIKE_DATA, else $XDG_DATA_HOME/klondike-solitaire, else ~/.local/share/klondike-solitaire
 *             stats.json      the statistics (stats.hpp)
 *             game.json       the game in progress (Game::save), written after each move by the UI, removed when
 *                             the game ends
 *   config  $XDG_CONFIG_HOME/klondike-solitaire, else ~/.config/klondike-solitaire
 *             settings.json   {"version":1,"rules":"draw=1,passes=0,..."}  the rules for the next deal
 *
 * Every file is written atomically (fsutil.hpp: temp file, fsync, rename), so a power cut keeps the last complete
 * file. A file that cannot be read back (cut, edited, written by a newer version, or a saved game that does not
 * replay) is renamed "<name>.bad" (replacing an older one) and Load::Bad is returned: the app starts that part
 * afresh, and last_error() says why (for the app's one log line). Missing = Load::Missing (first run).
 * Not thread safe; the UI calls it from one thread (the files are a few KB: a save takes about a millisecond plus
 * the fsync).
 */
#pragma once

#include "game.hpp"
#include "rules.hpp"
#include "stats.hpp"

#include <string>

namespace klondike {

class Store {
public:
    enum class Load { Ok, Missing, Bad };

    Store();
    Store(std::string data_dir, std::string config_dir);
    static std::string default_data_dir();
    static std::string default_config_dir();
    const std::string &data_dir() const { return data_dir_; }
    const std::string &config_dir() const { return config_dir_; }

    Load load_stats(Stats &out);
    bool save_stats(const Stats &stats);
    Load load_game(Game &out);
    bool save_game(const Game &game);
    bool clear_game();
    Load load_rules(Rules &out);
    bool save_rules(const Rules &rules);

    const std::string &last_error() const { return last_error_; }

private:
    Load bad(const std::string &path, const std::string &why);
    bool write(const std::string &dir, const std::string &name, const std::string &data);

    std::string data_dir_;
    std::string config_dir_;
    std::string last_error_;
};

} // namespace klondike
