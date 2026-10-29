/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: one game with history, score and time (see game.hpp).
 */
#include "game.hpp"

#include "json.hpp"
#include "solver.hpp"

#include <cstdio>

namespace klondike {

constexpr int kSaveVersion = 1;

Game::Game()
{
    new_game(0, Rules());
}

void Game::new_game(std::uint32_t number, const Rules &rules)
{
    start_ = deal_board(number);
    board_ = start_;
    rules_ = rules;
    number_ = number;
    user_number_ = number;
    winnable_ = false;
    history_.clear();
    redo_.clear();
    points_ = 0;
    elapsed_ms_ = 0;
    moves_made_ = 0;
}

bool Game::new_game(const Board &start, const Rules &rules, std::uint32_t number)
{
    if (!start.valid()) return false;
    new_game(number, rules);
    start_ = start;
    board_ = start;
    return true;
}

void Game::set_labels(std::uint32_t user_number, bool winnable)
{
    user_number_ = user_number;
    winnable_ = winnable;
}

std::int32_t Game::points_after(const Move &m, bool flipped, std::int32_t before) const
{
    if (rules_.scoring != Scoring::Standard) return 0;
    int delta = 0;
    switch (kind_of(m)) {
    case MoveKind::WasteToTableau: delta = score::kWasteToTableau; break;
    case MoveKind::WasteToFoundation: delta = score::kWasteToFoundation; break;
    case MoveKind::TableauToFoundation: delta = score::kTableauToFoundation; break;
    case MoveKind::FoundationToTableau: delta = score::kFoundationToTableau; break;
    case MoveKind::Recycle: delta = rules_.draw == 1 ? score::kRecycleDrawOne : score::kRecycleDrawThree; break;
    default: break;
    }
    if (flipped) delta += score::kTurnOverTableauCard;
    const std::int32_t after = before + delta;
    return after < 0 ? 0 : after;
}

void Game::play(const Move &m, bool chained)
{
    Step st;
    st.move = m;
    st.chained = chained && !history_.empty();
    st.score_before = points_;
    do_move(board_, rules_, m, &st.flipped);
    points_ = points_after(m, st.flipped, points_);
    history_.push_back(st);
}

bool Game::apply(const Move &m, bool chained)
{
    if (!can_move(board_, rules_, m)) return false;
    play(m, chained);
    redo_.clear();
    ++moves_made_;
    return true;
}

bool Game::undo()
{
    if (history_.empty()) return false;
    for (;;) {
        const Step st = history_.back();
        history_.pop_back();
        undo_move(board_, st.move, st.flipped);
        points_ = st.score_before;
        redo_.push_back(st);
        if (!st.chained || history_.empty()) break;
    }
    ++moves_made_;
    return true;
}

bool Game::redo()
{
    if (redo_.empty()) return false;
    bool first = true;
    while (!redo_.empty() && (first || redo_.back().chained)) {
        const Step st = redo_.back();
        if (!can_move(board_, rules_, st.move)) { /* cannot happen: the list is cleared by every new move */
            redo_.clear();
            break;
        }
        redo_.pop_back();
        play(st.move, st.chained);
        first = false;
    }
    if (first) return false;
    ++moves_made_;
    return true;
}

std::vector<Move> Game::legal_moves() const
{
    std::vector<Move> out;
    klondike::legal_moves(board_, rules_, out);
    return out;
}

bool Game::dead_end(std::uint32_t nodes) const
{
    if (board_.won()) return false;
    std::vector<Move> moves;
    klondike::legal_moves(board_, rules_, moves);
    bool others = false; /* tableau moves that turn nothing over, foundation -> tableau */
    for (const Move &m : moves) {
        switch (kind_of(m)) {
        case MoveKind::WasteToFoundation:
        case MoveKind::WasteToTableau:
        case MoveKind::TableauToFoundation: return false;
        case MoveKind::TableauToTableau: {
            const int s = m.from - kTableau0;
            const int base = board_.ntab[s] - m.count;
            if (base == board_.ndown[s] && base > 0) return false; /* turns a card over */
            others = true;
            break;
        }
        case MoveKind::FoundationToTableau: others = true; break;
        default: break;
        }
    }
    bool playable = false;
    for_each_reachable_talon_card(board_, rules_, [&](Card c, int, int) {
        if (playable) return;
        if (foundation_for(board_, c) >= 0) playable = true;
        for (int t = 0; t < kColumns && !playable; ++t)
            if (fits_column(board_, rules_, c, t)) playable = true;
    });
    if (playable) return false;
    if (!others) return true; /* only endless stock cycling is left */
    SolverLimits lim;
    lim.max_nodes = nodes;
    Solver solver(lim);
    return solver.solve(board_, rules_).verdict == Verdict::NotWinnable;
}

int Game::recycles_left() const
{
    const int max_rec = rules_.max_recycles();
    if (max_rec < 0) return -1;
    const int left = max_rec - board_.recycles;
    return left < 0 ? 0 : left;
}

void Game::add_time(std::uint32_t ms)
{
    if (!board_.won()) elapsed_ms_ += ms;
}

std::int32_t Game::score() const
{
    switch (rules_.scoring) {
    case Scoring::None: return 0;
    case Scoring::Vegas: return score::vegas(board_.foundation_cards());
    case Scoring::Standard: break;
    }
    std::int64_t s = points_;
    if (rules_.timed) {
        s -= score::time_penalty(elapsed_s());
        if (s < 0) s = 0;
        if (board_.won()) s += score::time_bonus(elapsed_s());
    }
    if (s > 0x7FFFFFFF) s = 0x7FFFFFFF;
    return static_cast<std::int32_t>(s);
}

GameRecord Game::record() const
{
    GameRecord r;
    r.won = won();
    r.score = score();
    r.seconds = elapsed_s();
    r.moves = moves_made_;
    return r;
}

/* ------------------------------------------------------------------------------------------------- save / load */

static std::string hex64(std::uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

static std::string step_token(const Step &st)
{
    return encode_move(st.move) + (st.chained ? "a" : "");
}

static bool parse_step(const std::string &token, Move &m, bool &chained)
{
    std::string t = token;
    chained = !t.empty() && t.back() == 'a';
    if (chained) t.pop_back();
    return parse_move(t, m);
}

std::string Game::save() const
{
    std::string s = "{\n";
    s += "  \"version\": " + std::to_string(kSaveVersion) + ",\n";
    s += "  \"rules\": " + json::quote(rules_.serialize()) + ",\n";
    s += "  \"deal\": " + std::to_string(number_) + ",\n";
    s += "  \"user_deal\": " + std::to_string(user_number_) + ",\n";
    s += std::string("  \"winnable\": ") + (winnable_ ? "true" : "false") + ",\n";
    s += "  \"start\": " + json::quote(start_.to_text()) + ",\n";
    auto list = [](const std::vector<Step> &steps) {
        std::string l = "[";
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (i) l += i % 16 ? ", " : ",\n    ";
            l += json::quote(step_token(steps[i]));
        }
        return l + "]";
    };
    s += "  \"history\": " + list(history_) + ",\n";
    s += "  \"redo\": " + list(redo_) + ",\n";
    s += "  \"elapsed_ms\": " + std::to_string(elapsed_ms_) + ",\n";
    s += "  \"moves\": " + std::to_string(moves_made_) + ",\n";
    s += "  \"position\": " + json::quote(hex64(board_.hash())) + "\n";
    s += "}\n";
    return s;
}

bool Game::load(const std::string &text, Game &out, std::string *why)
{
    auto fail = [why](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    json::Value root;
    std::string err;
    if (!json::parse(text, root, &err)) return fail("not JSON: " + err);
    if (root.kind != json::Value::Kind::Object) return fail("not an object");
    const json::Value *v = root.get("version");
    if (!v || !v->is_int()) return fail("no version");
    if (v->as_int() > kSaveVersion) return fail("written by a newer version");
    if (v->as_int() < 1) return fail("bad version");

    Rules rules;
    v = root.get("rules");
    if (!v || v->kind != json::Value::Kind::String || !Rules::parse(v->text, rules)) return fail("bad rules");
    auto get_u32 = [&root](const char *key, std::uint32_t &dst) {
        const json::Value *x = root.get(key);
        if (!x || !x->is_int() || x->as_int() < 0 || x->as_int() > 0xFFFFFFFFll) return false;
        dst = static_cast<std::uint32_t>(x->as_int());
        return true;
    };
    std::uint32_t number = 0, user = 0, moves = 0;
    if (!get_u32("deal", number)) return fail("bad deal number");
    if (!get_u32("user_deal", user)) user = number;
    if (!get_u32("moves", moves)) return fail("bad moves counter");
    v = root.get("elapsed_ms");
    if (!v || !v->is_int() || v->as_int() < 0) return fail("bad time");
    const std::uint64_t elapsed = static_cast<std::uint64_t>(v->as_int());
    v = root.get("start");
    Board start;
    std::string bwhy;
    if (!v || v->kind != json::Value::Kind::String || !Board::from_text(v->text, start, &bwhy))
        return fail("bad start position: " + bwhy);

    Game g;
    if (!g.new_game(start, rules, number)) return fail("bad start position");
    g.set_labels(user, root.get("winnable") && root.get("winnable")->as_bool());

    auto steps = [&](const char *key, std::vector<std::pair<Move, bool>> &dst) {
        const json::Value *a = root.get(key);
        if (!a || a->kind != json::Value::Kind::Array) return false;
        for (const json::Value &it : a->items) {
            Move m;
            bool chained = false;
            if (it.kind != json::Value::Kind::String || !parse_step(it.text, m, chained)) return false;
            dst.emplace_back(m, chained);
        }
        return true;
    };
    std::vector<std::pair<Move, bool>> hist, redo;
    if (!steps("history", hist)) return fail("bad history");
    if (!steps("redo", redo)) return fail("bad redo list");
    for (std::size_t i = 0; i < hist.size(); ++i) {
        if (!g.can_apply(hist[i].first)) return fail("history move " + std::to_string(i + 1) + " does not replay");
        g.play(hist[i].first, hist[i].second);
    }
    /* the redo list must replay too (from its back, as redo() takes it) */
    Game probe = g;
    for (std::size_t i = redo.size(); i-- > 0;) {
        if (!probe.can_apply(redo[i].first)) return fail("redo move does not replay");
        probe.play(redo[i].first, redo[i].second);
    }
    for (const auto &r : redo) {
        Step st;
        st.move = r.first;
        st.chained = r.second;
        g.redo_.push_back(st);
    }
    v = root.get("position");
    if (v && v->kind == json::Value::Kind::String && v->text != hex64(g.board_.hash()))
        return fail("position check does not match");
    g.elapsed_ms_ = elapsed;
    g.moves_made_ = moves;
    out = std::move(g);
    return true;
}

} // namespace klondike
