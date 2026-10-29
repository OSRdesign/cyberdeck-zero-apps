/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: one game (no LVGL; tests/test_core.cpp).
 *
 *   Lifecycle    Ready (nothing dug: no mines yet, the clock is stopped) -> Playing (after the first reveal: the mines
 *                are laid then, away from the first cell and its neighbours, see generator.hpp) -> Won (every safe
 *                cell is open: the remaining mines get flags) or Lost (a mine was opened). A finished game ignores
 *                further actions. There is no undo.
 *   Actions      reveal(x, y), toggle_flag(x, y), chord(x, y); each reports what happened, and last_opened() lists the
 *                cells that action opened (the clicked one first), for the UI's animation. A flagged cell cannot be
 *                revealed. Flags may be set before the first reveal. chord(): on a revealed number whose adjacent
 *                flags equal it, opens the other hidden neighbours (a wrong flag means a mine opens: lost); with a
 *                different number of flags nothing happens and FlagMismatch is reported.
 *   Loss         exploded() holds the mine cell(s) the player opened (the first is "the" exploded cell),
 *                mine_reveal_order() the other mines that are still hidden and unflagged, nearest to the exploded
 *                cell first (the UI's sequence), wrong_flags() the flagged cells that are not mines.
 *   Mines left   mines_left() = mines - flags, negative when more flags than mines are down.
 *   Clock        integer milliseconds, fed by the UI: tick(dt_ms) adds time while the game is Playing and not paused
 *                (the UI pauses it when the app is not in front); it starts at the first reveal and stops at the end.
 *                Hints add 10 s each (hint_penalty_ms): time_ms() = clock + penalty is what a result is measured by;
 *                display_seconds() is time_ms() in whole seconds capped at 999.
 *   Hints        hint() = a provably safe cell, else a provable mine (solver.hpp), else None; it only points, the UI
 *                then reveals / flags. A given hint counts in hints_used / penalty_ms and makes the game assisted,
 *                which is never eligible for a best time. None, Unavailable (not Playing) cost nothing.
 *   Prepare      the first reveal lays the mines, which with fair boards can take a while: prepare() does that part with no
 *                access to a Game (a static, pure function, safe on another thread), and supply() hands the result to a
 *                Game still in Ready, whose first reveal of that very cell then uses it instead of generating again.
 *   Save / load  save() / load(): the game in progress as JSON (the layout included), version 1; load() checks it
 *                all and returns false with a reason for anything inconsistent.
 */
#pragma once

#include "board.hpp"
#include "generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace minefield {

constexpr std::uint32_t kHintPenaltyMs = 10000;
constexpr std::uint32_t kDisplayCapSeconds = 999;

/* whole seconds of a time in ms, capped at 999 (the timer's three digits) */
std::uint32_t display_seconds(std::uint64_t ms);

struct GameConfig {
    Dims dims;                    /* clamped by Game */
    std::uint64_t seed = 1;
    bool fair = false;            /* lay a board the solver can finish without guessing */
    Preset preset = Preset::Beginner;   /* the difficulty it belongs to (statistics, resume rule) */
};

/* What Stats::record() needs when a game ends (or is given up). */
struct GameRecord {
    Dims dims;
    Preset preset = Preset::Beginner;
    std::uint64_t seed = 0;
    bool fair = false;
    bool fair_failed = false;
    bool started = false;         /* a reveal was made */
    bool won = false;
    bool assisted = false;        /* a hint was used */
    std::uint32_t time_ms = 0;    /* clock + hint penalty */
    int hints_used = 0;
};

class Game {
public:
    enum class Status { Ready, Playing, Won, Lost };
    enum class Outcome { Ignored, Opened, Exploded, Won };
    enum class ChordOutcome { Ignored, FlagMismatch, NothingToOpen, Opened, Exploded, Won };
    enum class HintKind { None, Safe, Mine, Unavailable };
    struct Hint {
        HintKind kind = HintKind::None;
        int cell = -1;
    };

    /* What prepare() makes: the layout for a first click (valid = it was made). */
    struct Prepared {
        bool valid = false;
        int first = -1;
        GenResult gen;
    };
    /* The layout a game of `cfg` gets when `first` (an index) is its first reveal: exactly what reveal() would lay. Pure. */
    static Prepared prepare(const GameConfig &cfg, int first);
    /* Keeps the layout for the first reveal of `p.first`; ignored unless the game is Ready and the cell is on the board. */
    void supply(const Prepared &p);

    Game();
    explicit Game(const GameConfig &cfg);
    void new_game(const GameConfig &cfg);

    /* ---- actions */
    Outcome reveal(int x, int y);
    bool toggle_flag(int x, int y);       /* false: ignored (revealed cell, finished game, outside) */
    ChordOutcome chord(int x, int y);
    Hint hint();
    const std::vector<int> &last_opened() const { return last_opened_; }

    /* ---- clock */
    void tick(std::uint32_t dt_ms);
    void set_paused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    bool clock_running() const { return status_ == Status::Playing && !paused_; }
    std::uint32_t clock_ms() const { return clock_ms_; }
    std::uint32_t penalty_ms() const { return penalty_ms_; }
    std::uint32_t time_ms() const;
    std::uint32_t display_seconds() const { return minefield::display_seconds(time_ms()); }

    /* ---- state */
    Status status() const { return status_; }
    bool finished() const { return status_ == Status::Won || status_ == Status::Lost; }
    bool started() const { return status_ != Status::Ready; }
    const GameConfig &config() const { return cfg_; }
    const Board &board() const { return board_; }
    int width() const { return board_.width(); }
    int height() const { return board_.height(); }
    int mines_left() const { return board_.mines() - board_.flag_count(); }
    int hints_used() const { return hints_used_; }
    bool assisted() const { return hints_used_ > 0; }
    int first_click() const { return first_click_; }
    bool fair_failed() const { return fair_failed_; }
    int attempts() const { return attempts_; }
    std::uint64_t seed_used() const { return seed_used_; }
    const std::vector<int> &exploded() const { return exploded_; }
    std::vector<int> mine_reveal_order() const;
    std::vector<int> wrong_flags() const;
    View view() const { return board_.view(); }
    GameRecord record() const;

    /* ---- save / load */
    std::string save() const;
    static bool load(const std::string &text, Game &out, std::string *why = nullptr);

private:
    void start(int first);
    void end_check(bool exploded_now);

    GameConfig cfg_;
    Board board_;
    Status status_ = Status::Ready;
    bool paused_ = false;
    std::uint32_t clock_ms_ = 0;
    std::uint32_t penalty_ms_ = 0;
    int hints_used_ = 0;
    int first_click_ = -1;
    bool fair_failed_ = false;
    int attempts_ = 0;
    std::uint64_t seed_used_ = 0;
    std::vector<int> exploded_;
    std::vector<int> last_opened_;
    Prepared prepared_;
};

} // namespace minefield
