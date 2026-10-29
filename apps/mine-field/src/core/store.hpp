/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: where the app keeps its files (no LVGL; tests/test_store.cpp). Same layout and rules as
 * Klondike's store. Two folders (injectable for the tests, created on the first save):
 *
 *   data    $MINEFIELD_DATA, else $XDG_DATA_HOME/mine-field, else ~/.local/share/mine-field
 *             stats.json      the statistics (stats.hpp)
 *             game.json       the game in progress (Game::save), written by the Session after each action, removed
 *                             when the game ends
 *   config  $XDG_CONFIG_HOME/mine-field, else ~/.config/mine-field
 *             settings.json   the settings (settings.hpp)
 *
 * Every file is written atomically (fsutil.hpp: temp file, fsync, rename), so a power cut keeps the last complete file.
 * A file that cannot be read back (cut, edited, a version this build does not know, a saved game that is inconsistent)
 * is renamed "<name>.bad" (replacing an older one) and Load::Bad is returned: the app starts that part afresh and
 * last_error() says why (for the app's one log line). Missing = Load::Missing (first run). Not thread safe.
 */
#pragma once

#include "game.hpp"
#include "settings.hpp"
#include "stats.hpp"

#include <string>

namespace minefield {

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
    Load load_settings(Settings &out);
    bool save_settings(const Settings &settings);

    const std::string &last_error() const { return last_error_; }

private:
    Load bad(const std::string &path, const std::string &why);
    bool write(const std::string &dir, const std::string &name, const std::string &data);

    std::string data_dir_;
    std::string config_dir_;
    std::string last_error_;
};

} // namespace minefield
