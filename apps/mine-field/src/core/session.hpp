/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: what the screen needs besides the game, with no LVGL (tests/test_store.cpp): the settings, the
 * statistics, the saved game, and the rules about when a game counts.
 *
 *   Session      owns the Game, the Stats, the Settings and the Store.
 *     Session(store, entropy)   `entropy` is whatever the UI has (time, pid); the core reads no system entropy. Seeds of
 *                               new games come from a deterministic generator started with it.
 *     load()           stats and settings, then the game in progress (resumed: returns true) or a fresh game on the
 *                      settings' board. A file that cannot be read is started afresh (Store sets it aside as .bad).
 *     new_game(seed)   a new game on the settings' board (seed 0 = a fresh random seed); the game it replaces counts as
 *                      a loss when a cell was dug (Game::started()) and it was not finished; a finished game was
 *                      recorded at once. A game not yet started costs nothing.
 *     reveal / flag / chord   forward to the Game; a win or a loss is recorded in the statistics at once, the saved
 *                      game removed; else the game is saved after every action that changed something.
 *     tick(ms), set_paused    the clock (Game::tick), never saved by itself: save_game() is for the UI to call when it
 *                      leaves the front or quits.
 *     hint()           Game::hint(), saved when given (its penalty counts).
 *   Boards           the screen decides the three boards (view_layout.hpp derive_board): the UI gives them with
 *                    set_screen_boards() before load(); dims_for(preset) answers. A game that is saved keeps its own
 *                    dims; load() / set_screen_boards() DROP a game whose dims differ from what the screen gives its
 *                    difficulty now (the screen changed shape): no loss is recorded, saved_game_dropped() says so once.
 *   The UI never changes the board of a game in progress: set_settings() changes the NEXT game's board only.
 */
#pragma once

#include "game.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "store.hpp"

#include <array>
#include <cstdint>

namespace minefield {

class Session {
public:
    explicit Session(Store store, std::uint64_t entropy = 0x4D494E45ull);

    bool load();

    /* The three boards of this screen (Beginner, Intermediate, Expert). A game still Ready, or a finished one, is replaced by
     * a fresh game on the new board when its dims differ; one in progress is dropped (no record). Returns true when a game in
     * progress was dropped. */
    bool set_screen_boards(const std::array<Dims, kRecordedPresets> &boards);
    Dims dims_for(Preset p) const { return boards_[static_cast<std::size_t>(p)]; }
    /* load() or set_screen_boards() dropped a saved game for its shape; reading it clears it */
    bool take_dropped_notice();

    Game &game() { return game_; }
    const Game &game() const { return game_; }
    const Settings &settings() const { return settings_; }
    void set_settings(const Settings &s);
    const Stats &stats() const { return stats_; }
    void reset_stats(Preset p);
    void reset_all_stats();
    Store &store() { return store_; }

    /* a cell was dug and the game is not over: leaving it counts as a loss */
    bool in_progress() const { return game_.status() == Game::Status::Playing; }
    /* the next game's board differs from this game's */
    bool settings_pending() const;

    std::uint64_t random_seed();
    void new_game(std::uint64_t seed = 0);
    /* A new game with exactly the configuration `cfg` (board, seed, fair: "same board", "try again"), whatever the
     * settings say now; the game it replaces is treated as by new_game(). */
    void replay(const GameConfig &cfg);

    Game::Outcome reveal(int x, int y);
    bool toggle_flag(int x, int y);
    Game::ChordOutcome chord(int x, int y);
    Game::Hint hint();
    void tick(std::uint32_t ms) { game_.tick(ms); }
    void set_paused(bool p) { game_.set_paused(p); }
    void save_game();

private:
    void abandon_current();
    void after_action();

    Store store_;
    Game game_;
    Stats stats_;
    Settings settings_;
    bool recorded_ = false;
    bool dropped_ = false;
    std::array<Dims, kRecordedPresets> boards_{Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}};
    std::uint64_t rng_;
};

} // namespace minefield
