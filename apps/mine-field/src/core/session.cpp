/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the session (see session.hpp).
 */
#include "session.hpp"

#include "rng.hpp"

namespace minefield {

Session::Session(Store store, std::uint64_t entropy) : store_(std::move(store)), rng_(entropy) {}

static GameConfig config_for(const Settings &s, const Dims &dims, std::uint64_t seed)
{
    GameConfig c;
    c.dims = dims;
    c.preset = s.preset;
    c.seed = seed;
    c.fair = s.fair;
    return c;
}

bool Session::load()
{
    store_.load_stats(stats_);
    store_.load_settings(settings_);
    Game g;
    if (store_.load_game(g) == Store::Load::Ok && g.status() == Game::Status::Playing) {
        if (g.config().dims == dims_for(g.config().preset)) {
            game_ = g;
            recorded_ = false;
            return true;
        }
        store_.clear_game();   /* the screen changed shape: dropped, never a loss */
        dropped_ = true;
    }
    game_.new_game(config_for(settings_, dims_for(settings_.preset), random_seed()));
    recorded_ = false;
    return false;
}

void Session::set_settings(const Settings &s)
{
    settings_ = s;
    store_.save_settings(settings_);
}

bool Session::settings_pending() const
{
    return dims_for(settings_.preset) != game_.config().dims || settings_.fair != game_.config().fair;
}

bool Session::set_screen_boards(const std::array<Dims, kRecordedPresets> &boards)
{
    boards_ = boards;
    if (game_.config().dims == dims_for(game_.config().preset)) return false;
    const bool was_playing = game_.status() == Game::Status::Playing;
    game_.new_game(config_for(settings_, dims_for(settings_.preset), random_seed()));
    recorded_ = false;
    if (was_playing) {
        store_.clear_game();
        dropped_ = true;
    }
    return was_playing;
}

bool Session::take_dropped_notice()
{
    const bool d = dropped_;
    dropped_ = false;
    return d;
}

void Session::reset_stats(Preset p)
{
    stats_.reset(p);
    store_.save_stats(stats_);
}

void Session::reset_all_stats()
{
    stats_.reset_all();
    store_.save_stats(stats_);
}

std::uint64_t Session::random_seed()
{
    Rng r(rng_);
    const std::uint64_t v = r.next();
    rng_ = r.next() ^ v;
    return v ? v : 1;
}

void Session::abandon_current()
{
    if (!in_progress() || recorded_) return;
    GameRecord rec = game_.record();
    rec.won = false;
    stats_.record(rec);
    store_.save_stats(stats_);
}

void Session::new_game(std::uint64_t seed)
{
    abandon_current();
    game_.new_game(config_for(settings_, dims_for(settings_.preset), seed ? seed : random_seed()));
    recorded_ = false;
    store_.clear_game();
}

void Session::replay(const GameConfig &cfg)
{
    abandon_current();
    game_.new_game(cfg);
    recorded_ = false;
    store_.clear_game();
}

void Session::after_action()
{
    if (game_.finished()) {
        if (!recorded_) {
            stats_.record(game_.record());
            store_.save_stats(stats_);
            store_.clear_game();
            recorded_ = true;
        }
    } else {
        save_game();
    }
}

void Session::save_game()
{
    if (game_.status() == Game::Status::Playing) store_.save_game(game_);
}

Game::Outcome Session::reveal(int x, int y)
{
    const Game::Outcome o = game_.reveal(x, y);
    if (o != Game::Outcome::Ignored) after_action();
    return o;
}

bool Session::toggle_flag(int x, int y)
{
    const bool r = game_.toggle_flag(x, y);
    if (r) after_action();
    return r;
}

Game::ChordOutcome Session::chord(int x, int y)
{
    const Game::ChordOutcome o = game_.chord(x, y);
    if (o == Game::ChordOutcome::Opened || o == Game::ChordOutcome::Exploded || o == Game::ChordOutcome::Won)
        after_action();
    return o;
}

Game::Hint Session::hint()
{
    const Game::Hint h = game_.hint();
    if (h.kind == Game::HintKind::Safe || h.kind == Game::HintKind::Mine) after_action();
    return h;
}

} // namespace minefield
