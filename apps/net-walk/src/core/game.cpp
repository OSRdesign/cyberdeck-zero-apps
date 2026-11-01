/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: one game (see game.hpp).
 */
#include "game.hpp"

#include <cstdio>

namespace netwalk {

namespace {

const LevelInfo kLevels[kLevelCount] = {
    {"easy", "Easy", "5x5", 5, 5, false},
    {"normal", "Normal", "7x7", 7, 7, false},
    {"hard", "Hard", "9x9", 9, 9, false},
    {"expert", "Expert", "9x9 wrap", 9, 9, true},
};

} // namespace

const LevelInfo &level_info(Level l)
{
    return kLevels[static_cast<int>(l)];
}

Level level_from_key(const std::string &key, bool *found)
{
    for (int i = 0; i < kLevelCount; ++i)
        if (key == kLevels[i].key) {
            if (found) *found = true;
            return static_cast<Level>(i);
        }
    if (found) *found = false;
    return Level::Normal;
}

void Game::start(Level level, std::uint64_t seed)
{
    const LevelInfo &li = level_info(level);
    start_with(level, seed, Board::generate(li.w, li.h, li.wrap, seed));
}

void Game::start_with(Level level, std::uint64_t seed, const Board &board)
{
    level_ = level;
    seed_ = seed;
    board_ = board;
    moves_ = 0;
    elapsed_ms_ = 0;
    won_ = false;
    refresh();
}

void Game::refresh()
{
    powered_ = board_.powered();
    won_ = board_.is_won();
}

bool Game::turn(int i, bool clockwise)
{
    if (won_ || i < 0 || i >= board_.size() || board_.locked(i)) return false;
    if (!board_.rotate(i, clockwise)) return false;
    ++moves_;
    refresh();
    return true;
}

bool Game::toggle_lock(int i)
{
    if (won_ || i < 0 || i >= board_.size()) return false;
    board_.set_locked(i, !board_.locked(i));
    return true;
}

void Game::restore(int moves, std::uint32_t elapsed_ms)
{
    moves_ = moves;
    elapsed_ms_ = elapsed_ms;
    refresh();
}

void Game::tick(std::uint32_t dt_ms)
{
    if (started() && !won_) elapsed_ms_ += dt_ms;
}

std::string format_time(int seconds)
{
    if (seconds < 0) seconds = 0;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02d:%02d", seconds / 60, seconds % 60);
    return buf;
}

} // namespace netwalk
