/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the board (no LVGL; tests/test_core.cpp). Cells are numbered row by row, index = y * width + x.
 *
 *   Dims / presets   Beginner, Intermediate, Expert: their dimensions come from the screen (view_layout.hpp), not from
 *                    a table. clamp_dims() keeps a board inside 5..30 wide, 5..30 high and 1 .. w*h-9 mines (the first dug
 *                    cell and its eight neighbours are always free of mines, so at most w*h-9 can be placed).
 *   Board            the cells: hidden / revealed / flagged, the mine layout (placed on the first reveal: see
 *                    generator.hpp, Game), the adjacent mine counts. reveal() opens a cell and floods through zero
 *                    cells with an explicit stack (no recursion: 30x30 is fine); flags stop the flood.
 *   View             what the player can see (the solver's input): hidden, flagged, or the number of a revealed cell.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minefield {

constexpr int kMinDim = 5;
constexpr int kMaxDim = 30;
constexpr int kMaxCells = kMaxDim * kMaxDim;

enum class Preset : std::uint8_t { Beginner = 0, Intermediate = 1, Expert = 2 };
constexpr int kRecordedPresets = 3;   /* every difficulty has statistics and best times */

struct Dims {
    int w = 9, h = 9, mines = 10;
    bool operator==(const Dims &o) const { return w == o.w && h == o.h && mines == o.mines; }
    bool operator!=(const Dims &o) const { return !(*this == o); }
};

int max_mines(int w, int h);                /* w*h - 9 */
Dims clamp_dims(int w, int h, int mines);   /* w, h into 5..30; mines into 1 .. max_mines */
/* The fixed boards of version 1 of the app (9x9/10, 16x16/40, 30x16/99): only to recognise an old saved game. */
Dims legacy_preset_dims(Preset p);
bool legacy_preset_of(const Dims &d, Preset &out);
const char *preset_name(Preset p);          /* "beginner" "intermediate" "expert" */
bool parse_preset(const std::string &name, Preset &out);

enum class CellState : std::uint8_t { Hidden = 0, Revealed = 1, Flagged = 2 };

constexpr std::int8_t kViewHidden = -1;
constexpr std::int8_t kViewFlagged = -2;

struct View {
    int w = 0, h = 0, mines = 0;
    std::vector<std::int8_t> cell;           /* kViewHidden, kViewFlagged, or 0..8 for a revealed cell */
    int size() const { return w * h; }
};

enum class RevealResult { NoOp, Opened, Mine };

class Board {
public:
    Board() : Board(9, 9, 10) {}
    Board(int w, int h, int mines);

    int width() const { return w_; }
    int height() const { return h_; }
    int mines() const { return mines_; }
    int size() const { return w_ * h_; }
    int index(int x, int y) const { return y * w_ + x; }
    int x_of(int i) const { return i % w_; }
    int y_of(int i) const { return i / w_; }
    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    /* The cells around i (up to 8), in row-major order. Returns the count; `out` holds at least 8. */
    int neighbours(int i, int *out) const;

    bool placed() const { return placed_; }
    /* Sets the mine layout (one byte per cell, 0 / 1; exactly mines() ones) and the adjacent counts; every cell goes
     * back to hidden. False when the layout does not fit. */
    bool place(const std::vector<std::uint8_t> &mine_map);
    /* Everything hidden again, the layout kept. */
    void reset_states();

    CellState state(int i) const { return state_[static_cast<std::size_t>(i)]; }
    bool is_mine(int i) const { return placed_ && mine_[static_cast<std::size_t>(i)] != 0; }
    int adjacent(int i) const { return adj_[static_cast<std::size_t>(i)]; }   /* 0 before place() */
    const std::vector<std::uint8_t> &mine_map() const { return mine_; }

    int flag_count() const { return flags_; }
    int revealed_safe() const { return revealed_safe_; }
    int safe_left() const { return size() - mines_ - revealed_safe_; }
    bool all_safe_revealed() const { return placed_ && safe_left() == 0; }

    /* Opens a hidden cell (not a flagged or revealed one: NoOp). A mine becomes revealed and the result is Mine; a
     * safe cell opens, flooding through zero cells; `opened` (optional) gets the cells opened, the clicked one
     * first. Needs place(). */
    RevealResult reveal(int i, std::vector<int> *opened = nullptr);
    /* Hidden <-> flagged; false for a revealed cell. */
    bool toggle_flag(int i);
    /* Flags every unflagged mine (what a won game shows). */
    void flag_all_mines();
    /* Restores a saved state (Game::load): one state per cell, counters recomputed. False when sizes differ. */
    bool restore_states(const std::vector<CellState> &states);

    View view() const;

private:
    int w_, h_, mines_;
    bool placed_ = false;
    int flags_ = 0;
    int revealed_safe_ = 0;
    std::vector<CellState> state_;
    std::vector<std::uint8_t> mine_;
    std::vector<std::uint8_t> adj_;
};

} // namespace minefield
