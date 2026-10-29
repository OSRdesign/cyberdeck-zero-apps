/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the board (see board.hpp).
 */
#include "board.hpp"

#include <algorithm>

namespace minefield {

Dims legacy_preset_dims(Preset p)
{
    switch (p) {
    case Preset::Intermediate: return Dims{16, 16, 40};
    case Preset::Expert: return Dims{30, 16, 99};
    default: return Dims{9, 9, 10};
    }
}

int max_mines(int w, int h)
{
    return w * h - 9;
}

Dims clamp_dims(int w, int h, int mines)
{
    Dims d;
    d.w = std::clamp(w, kMinDim, kMaxDim);
    d.h = std::clamp(h, kMinDim, kMaxDim);
    d.mines = std::clamp(mines, 1, max_mines(d.w, d.h));
    return d;
}

bool legacy_preset_of(const Dims &d, Preset &out)
{
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert})
        if (legacy_preset_dims(p) == d) {
            out = p;
            return true;
        }
    return false;
}

const char *preset_name(Preset p)
{
    switch (p) {
    case Preset::Beginner: return "beginner";
    case Preset::Intermediate: return "intermediate";
    case Preset::Expert: return "expert";
    default: return "beginner";
    }
}

bool parse_preset(const std::string &name, Preset &out)
{
    for (Preset p : {Preset::Beginner, Preset::Intermediate, Preset::Expert})
        if (name == preset_name(p)) {
            out = p;
            return true;
        }
    return false;
}

Board::Board(int w, int h, int mines)
    : w_(std::clamp(w, 1, kMaxDim)), h_(std::clamp(h, 1, kMaxDim)), mines_(std::max(0, mines))
{
    const std::size_t n = static_cast<std::size_t>(w_ * h_);
    state_.assign(n, CellState::Hidden);
    mine_.assign(n, 0);
    adj_.assign(n, 0);
}

int Board::neighbours(int i, int *out) const
{
    const int x = x_of(i), y = y_of(i);
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            if (in_bounds(x + dx, y + dy)) out[n++] = index(x + dx, y + dy);
        }
    return n;
}

bool Board::place(const std::vector<std::uint8_t> &mine_map)
{
    if (static_cast<int>(mine_map.size()) != size()) return false;
    int count = 0;
    for (std::uint8_t m : mine_map) count += m ? 1 : 0;
    if (count != mines_) return false;
    for (int i = 0; i < size(); ++i) mine_[static_cast<std::size_t>(i)] = mine_map[static_cast<std::size_t>(i)] ? 1 : 0;
    for (int i = 0; i < size(); ++i) {
        int nb[8];
        const int n = neighbours(i, nb);
        int c = 0;
        for (int k = 0; k < n; ++k) c += mine_[static_cast<std::size_t>(nb[k])];
        adj_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(c);
    }
    placed_ = true;
    reset_states();
    return true;
}

void Board::reset_states()
{
    std::fill(state_.begin(), state_.end(), CellState::Hidden);
    flags_ = 0;
    revealed_safe_ = 0;
}

bool Board::restore_states(const std::vector<CellState> &states)
{
    if (static_cast<int>(states.size()) != size()) return false;
    state_ = states;
    flags_ = 0;
    revealed_safe_ = 0;
    for (int i = 0; i < size(); ++i) {
        if (state_[static_cast<std::size_t>(i)] == CellState::Flagged) ++flags_;
        else if (state_[static_cast<std::size_t>(i)] == CellState::Revealed && !mine_[static_cast<std::size_t>(i)]) ++revealed_safe_;
    }
    return true;
}

RevealResult Board::reveal(int i, std::vector<int> *opened)
{
    if (!placed_ || i < 0 || i >= size() || state_[static_cast<std::size_t>(i)] != CellState::Hidden)
        return RevealResult::NoOp;
    if (mine_[static_cast<std::size_t>(i)]) {
        state_[static_cast<std::size_t>(i)] = CellState::Revealed;
        return RevealResult::Mine;
    }
    std::vector<int> stack;
    const auto open = [&](int c) {
        state_[static_cast<std::size_t>(c)] = CellState::Revealed;
        ++revealed_safe_;
        if (opened) opened->push_back(c);
        if (adj_[static_cast<std::size_t>(c)] == 0) stack.push_back(c);
    };
    open(i);
    while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        int nb[8];
        const int n = neighbours(c, nb);
        for (int k = 0; k < n; ++k)
            if (state_[static_cast<std::size_t>(nb[k])] == CellState::Hidden) open(nb[k]);   /* a zero cell has no mine around */
    }
    return RevealResult::Opened;
}

bool Board::toggle_flag(int i)
{
    if (i < 0 || i >= size()) return false;
    CellState &s = state_[static_cast<std::size_t>(i)];
    if (s == CellState::Revealed) return false;
    if (s == CellState::Hidden) {
        s = CellState::Flagged;
        ++flags_;
    } else {
        s = CellState::Hidden;
        --flags_;
    }
    return true;
}

void Board::flag_all_mines()
{
    for (int i = 0; i < size(); ++i)
        if (mine_[static_cast<std::size_t>(i)] && state_[static_cast<std::size_t>(i)] == CellState::Hidden) {
            state_[static_cast<std::size_t>(i)] = CellState::Flagged;
            ++flags_;
        }
}

View Board::view() const
{
    View v;
    v.w = w_;
    v.h = h_;
    v.mines = mines_;
    v.cell.resize(static_cast<std::size_t>(size()));
    for (int i = 0; i < size(); ++i) {
        const CellState s = state_[static_cast<std::size_t>(i)];
        std::int8_t c = kViewHidden;
        if (s == CellState::Flagged) c = kViewFlagged;
        else if (s == CellState::Revealed && !mine_[static_cast<std::size_t>(i)]) c = static_cast<std::int8_t>(adj_[static_cast<std::size_t>(i)]);
        v.cell[static_cast<std::size_t>(i)] = c;
    }
    return v;
}

} // namespace minefield
