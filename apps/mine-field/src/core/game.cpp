/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: one game (see game.hpp).
 */
#include "game.hpp"

#include "generator.hpp"
#include "json.hpp"
#include "solver.hpp"

#include <algorithm>
#include <cstdlib>

namespace minefield {

constexpr int kSaveVersion = 2;   /* 1 had no preset: it is read, the preset recognised from the old fixed boards */

std::uint32_t display_seconds(std::uint64_t ms)
{
    const std::uint64_t s = ms / 1000;
    return static_cast<std::uint32_t>(s > kDisplayCapSeconds ? kDisplayCapSeconds : s);
}

static GameConfig clamped(const GameConfig &c)
{
    GameConfig r = c;
    r.dims = clamp_dims(c.dims.w, c.dims.h, c.dims.mines);
    return r;
}

Game::Game() : Game(GameConfig()) {}

Game::Game(const GameConfig &cfg) : cfg_(clamped(cfg)), board_(cfg_.dims.w, cfg_.dims.h, cfg_.dims.mines)
{
    seed_used_ = cfg_.seed;
}

void Game::new_game(const GameConfig &cfg)
{
    *this = Game(cfg);
}

std::uint32_t Game::time_ms() const
{
    const std::uint64_t t = static_cast<std::uint64_t>(clock_ms_) + penalty_ms_;
    return t > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(t);
}

void Game::tick(std::uint32_t dt_ms)
{
    if (!clock_running()) return;
    const std::uint64_t t = static_cast<std::uint64_t>(clock_ms_) + dt_ms;
    clock_ms_ = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(t);
}

Game::Prepared Game::prepare(const GameConfig &cfg, int first)
{
    const GameConfig c = clamped(cfg);
    Prepared p;
    if (first < 0 || first >= c.dims.w * c.dims.h) return p;
    p.gen = generate(c.dims.w, c.dims.h, c.dims.mines, c.seed, first, c.fair);
    p.first = first;
    p.valid = true;
    return p;
}

void Game::supply(const Prepared &p)
{
    if (status_ != Status::Ready || !p.valid || p.first < 0 || p.first >= board_.size()) return;
    prepared_ = p;
}

void Game::start(int first)
{
    GenResult g;
    if (prepared_.valid && prepared_.first == first) g = prepared_.gen;
    else g = generate(board_.width(), board_.height(), board_.mines(), cfg_.seed, first, cfg_.fair);
    prepared_ = Prepared();
    board_.place(g.mines);
    first_click_ = first;
    fair_failed_ = g.fair_failed;
    attempts_ = g.attempts;
    seed_used_ = g.seed_used;
    status_ = Status::Playing;
}

void Game::end_check(bool exploded_now)
{
    if (exploded_now) status_ = Status::Lost;
    else if (board_.all_safe_revealed()) {
        status_ = Status::Won;
        board_.flag_all_mines();
    }
}

Game::Outcome Game::reveal(int x, int y)
{
    last_opened_.clear();
    if (finished() || !board_.in_bounds(x, y)) return Outcome::Ignored;
    const int i = board_.index(x, y);
    if (board_.state(i) != CellState::Hidden) return Outcome::Ignored;
    if (status_ == Status::Ready) start(i);
    const RevealResult r = board_.reveal(i, &last_opened_);
    if (r == RevealResult::NoOp) return Outcome::Ignored;
    if (r == RevealResult::Mine) {
        exploded_.push_back(i);
        end_check(true);
        return Outcome::Exploded;
    }
    end_check(false);
    return status_ == Status::Won ? Outcome::Won : Outcome::Opened;
}

bool Game::toggle_flag(int x, int y)
{
    if (finished() || !board_.in_bounds(x, y)) return false;
    return board_.toggle_flag(board_.index(x, y));
}

Game::ChordOutcome Game::chord(int x, int y)
{
    last_opened_.clear();
    if (status_ != Status::Playing || !board_.in_bounds(x, y)) return ChordOutcome::Ignored;
    const int i = board_.index(x, y);
    if (board_.state(i) != CellState::Revealed || board_.adjacent(i) == 0) return ChordOutcome::Ignored;
    int nb[8];
    const int n = board_.neighbours(i, nb);
    int flags = 0;
    for (int k = 0; k < n; ++k)
        if (board_.state(nb[k]) == CellState::Flagged) ++flags;
    if (flags != board_.adjacent(i)) return ChordOutcome::FlagMismatch;
    bool any = false;
    for (int k = 0; k < n; ++k) {
        if (board_.state(nb[k]) != CellState::Hidden) continue;
        any = true;
        if (board_.reveal(nb[k], &last_opened_) == RevealResult::Mine) exploded_.push_back(nb[k]);
    }
    if (!any) return ChordOutcome::NothingToOpen;
    end_check(!exploded_.empty());
    if (status_ == Status::Lost) return ChordOutcome::Exploded;
    return status_ == Status::Won ? ChordOutcome::Won : ChordOutcome::Opened;
}

Game::Hint Game::hint()
{
    Hint h;
    if (status_ != Status::Playing) {
        h.kind = HintKind::Unavailable;
        return h;
    }
    const View v = board_.view();
    int c = find_safe_cell(v);
    if (c >= 0) {
        h.kind = HintKind::Safe;
        h.cell = c;
    } else if ((c = find_mine(v)) >= 0) {
        h.kind = HintKind::Mine;
        h.cell = c;
    } else {
        return h;
    }
    ++hints_used_;
    penalty_ms_ += kHintPenaltyMs;
    return h;
}

std::vector<int> Game::mine_reveal_order() const
{
    std::vector<int> out;
    if (status_ != Status::Lost || exploded_.empty()) return out;
    const int ex = board_.x_of(exploded_[0]), ey = board_.y_of(exploded_[0]);
    for (int i = 0; i < board_.size(); ++i)
        if (board_.is_mine(i) && board_.state(i) == CellState::Hidden) out.push_back(i);
    const auto d2 = [&](int i) {
        const int dx = board_.x_of(i) - ex, dy = board_.y_of(i) - ey;
        return dx * dx + dy * dy;
    };
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return d2(a) < d2(b); });
    return out;
}

std::vector<int> Game::wrong_flags() const
{
    std::vector<int> out;
    if (!board_.placed()) return out;
    for (int i = 0; i < board_.size(); ++i)
        if (board_.state(i) == CellState::Flagged && !board_.is_mine(i)) out.push_back(i);
    return out;
}

GameRecord Game::record() const
{
    GameRecord r;
    r.dims = cfg_.dims;
    r.preset = cfg_.preset;
    r.seed = cfg_.seed;
    r.fair = cfg_.fair;
    r.fair_failed = fair_failed_;
    r.started = started();
    r.won = status_ == Status::Won;
    r.assisted = assisted();
    r.time_ms = time_ms();
    r.hints_used = hints_used_;
    return r;
}

/* ---------------------------------------------------------------------------------------------- save / load */

std::string Game::save() const
{
    std::string s = "{\n  \"version\": " + std::to_string(kSaveVersion) + ",\n";
    s += "  \"w\": " + std::to_string(cfg_.dims.w) + ", \"h\": " + std::to_string(cfg_.dims.h) +
         ", \"mines\": " + std::to_string(cfg_.dims.mines) + ",\n";
    s += std::string("  \"preset\": \"") + preset_name(cfg_.preset) + "\",\n";
    s += "  \"seed\": \"" + std::to_string(cfg_.seed) + "\",\n";
    s += "  \"seed_used\": \"" + std::to_string(seed_used_) + "\",\n";
    s += std::string("  \"fair\": ") + (cfg_.fair ? "true" : "false") + ", \"fair_failed\": " +
         (fair_failed_ ? "true" : "false") + ", \"attempts\": " + std::to_string(attempts_) + ",\n";
    s += "  \"first_click\": " + std::to_string(first_click_) + ",\n";
    s += "  \"clock_ms\": " + std::to_string(clock_ms_) + ", \"penalty_ms\": " + std::to_string(penalty_ms_) +
         ", \"hints\": " + std::to_string(hints_used_) + ",\n";
    std::string mines, cells;
    for (int i = 0; i < board_.size(); ++i) {
        mines += board_.is_mine(i) ? '1' : '0';
        const CellState st = board_.state(i);
        cells += st == CellState::Hidden ? 'h' : st == CellState::Revealed ? 'r' : 'f';
    }
    s += "  \"mine_map\": \"" + mines + "\",\n  \"cells\": \"" + cells + "\"\n}\n";
    return s;
}

static bool get_u64(const json::Value &root, const char *key, std::uint64_t &out)
{
    const json::Value *v = root.get(key);
    if (!v || v->kind != json::Value::Kind::String || v->text.empty() || v->text.size() > 20) return false;
    std::uint64_t r = 0;
    for (char c : v->text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (r > (0xFFFFFFFFFFFFFFFFull - d) / 10) return false;
        r = r * 10 + d;
    }
    out = r;
    return true;
}

static bool get_int(const json::Value &root, const char *key, std::int64_t lo, std::int64_t hi, std::int64_t &out)
{
    const json::Value *v = root.get(key);
    if (!v || !v->is_int()) return false;
    out = v->as_int();
    return out >= lo && out <= hi;
}

bool Game::load(const std::string &text, Game &out, std::string *why)
{
    const auto fail = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    json::Value root;
    std::string jwhy;
    if (!json::parse(text, root, &jwhy)) return fail("not JSON");
    if (root.kind != json::Value::Kind::Object) return fail("not an object");
    std::int64_t ver, w, h, mines, first, clock, penalty, hints, attempts;
    if (!get_int(root, "version", 1, kSaveVersion, ver)) return fail("bad version");
    Preset preset = Preset::Beginner;
    if (ver >= 2) {
        const json::Value *pv = root.get("preset");
        if (!pv || pv->kind != json::Value::Kind::String || !parse_preset(pv->text, preset)) return fail("bad preset");
    }
    if (!get_int(root, "w", kMinDim, kMaxDim, w) || !get_int(root, "h", kMinDim, kMaxDim, h)) return fail("bad size");
    if (!get_int(root, "mines", 1, max_mines(static_cast<int>(w), static_cast<int>(h)), mines)) return fail("bad mine count");
    if (!get_int(root, "first_click", 0, w * h - 1, first)) return fail("bad first click");
    if (!get_int(root, "clock_ms", 0, 0xFFFFFFFFll, clock) || !get_int(root, "penalty_ms", 0, 0xFFFFFFFFll, penalty) ||
        !get_int(root, "hints", 0, 100000, hints) || !get_int(root, "attempts", 0, 1000000, attempts))
        return fail("bad counters");
    std::uint64_t seed, seed_used;
    if (!get_u64(root, "seed", seed) || !get_u64(root, "seed_used", seed_used)) return fail("bad seed");
    const json::Value *fair = root.get("fair"), *failed = root.get("fair_failed");
    const json::Value *mm = root.get("mine_map"), *cc = root.get("cells");
    if (!fair || fair->kind != json::Value::Kind::Bool || !failed || failed->kind != json::Value::Kind::Bool)
        return fail("bad flags");
    const std::size_t n = static_cast<std::size_t>(w * h);
    if (!mm || mm->kind != json::Value::Kind::String || mm->text.size() != n) return fail("bad mine map");
    if (!cc || cc->kind != json::Value::Kind::String || cc->text.size() != n) return fail("bad cells");

    GameConfig cfg;
    cfg.dims = Dims{static_cast<int>(w), static_cast<int>(h), static_cast<int>(mines)};
    cfg.seed = seed;
    cfg.fair = fair->boolean;
    if (ver < 2) legacy_preset_of(cfg.dims, preset);
    cfg.preset = preset;
    Game g(cfg);
    std::vector<std::uint8_t> map(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (mm->text[i] != '0' && mm->text[i] != '1') return fail("bad mine map");
        map[i] = mm->text[i] == '1';
    }
    if (!g.board_.place(map)) return fail("mine map does not match the mine count");
    std::vector<CellState> st(n);
    for (std::size_t i = 0; i < n; ++i) {
        switch (cc->text[i]) {
        case 'h': st[i] = CellState::Hidden; break;
        case 'f': st[i] = CellState::Flagged; break;
        case 'r':
            if (map[i]) return fail("a mine is revealed");
            st[i] = CellState::Revealed;
            break;
        default: return fail("bad cells");
        }
    }
    g.board_.restore_states(st);
    if (g.board_.state(static_cast<int>(first)) != CellState::Revealed) return fail("first click not revealed");
    if (g.board_.all_safe_revealed()) return fail("game already won");
    int nb[8];
    const int cnt = g.board_.neighbours(static_cast<int>(first), nb);
    for (int k = 0; k < cnt; ++k)
        if (map[static_cast<std::size_t>(nb[k])]) return fail("mine next to the first click");
    if (map[static_cast<std::size_t>(first)]) return fail("mine on the first click");
    g.status_ = Status::Playing;
    g.first_click_ = static_cast<int>(first);
    g.clock_ms_ = static_cast<std::uint32_t>(clock);
    g.penalty_ms_ = static_cast<std::uint32_t>(penalty);
    g.hints_used_ = static_cast<int>(hints);
    g.attempts_ = static_cast<int>(attempts);
    g.fair_failed_ = failed->boolean;
    g.seed_used_ = seed_used;
    out = g;
    return true;
}

} // namespace minefield
