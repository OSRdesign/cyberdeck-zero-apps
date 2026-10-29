/*
 * SPDX-License-Identifier: MIT
 */
#include "session.hpp"

#include "fsutil.hpp"
#include "json.hpp"
#include "solver.hpp"

#include <chrono>
#include <cstdio>
#include <random>

#include <unistd.h>

namespace klondike {

namespace {

const char *const kPrefsFile = "ui.json";

int clamp_int(std::int64_t v, int lo, int hi)
{
    return static_cast<int>(v < lo ? lo : (v > hi ? hi : v));
}

} // namespace

std::string prefs_to_json(const UiPrefs &p)
{
    std::string s = "{\"version\":1";
    s += ",\"left_handed\":" + std::string(p.left_handed ? "true" : "false");
    s += ",\"four_colour\":" + std::string(p.four_colour ? "true" : "false");
    s += ",\"felt\":" + std::to_string(p.felt);
    s += ",\"back\":" + std::to_string(p.back);
    s += ",\"anim\":" + std::to_string(p.anim);
    s += ",\"winnable\":" + std::string(p.winnable ? "true" : "false");
    s += "}\n";
    return s;
}

bool prefs_from_json(const std::string &text, UiPrefs &out)
{
    json::Value root;
    if (!json::parse(text, root) || root.kind != json::Value::Kind::Object) return false;
    UiPrefs p;
    if (const json::Value *v = root.get("left_handed")) p.left_handed = v->as_bool(p.left_handed);
    if (const json::Value *v = root.get("four_colour")) p.four_colour = v->as_bool(p.four_colour);
    if (const json::Value *v = root.get("felt")) p.felt = clamp_int(v->as_int(0), 0, 2);
    if (const json::Value *v = root.get("back")) p.back = clamp_int(v->as_int(0), 0, 1);
    if (const json::Value *v = root.get("anim")) p.anim = clamp_int(v->as_int(0), 0, 2);
    if (const json::Value *v = root.get("winnable")) p.winnable = v->as_bool(p.winnable);
    out = p;
    return true;
}

Session::Session(Store store) : store_(std::move(store))
{
    std::random_device rd;
    rng_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
           static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
           (static_cast<std::uint64_t>(::getpid()) << 20);
}

bool Session::load()
{
    store_.load_stats(stats_);
    if (store_.load_rules(rules_) != Store::Load::Ok) rules_ = Rules();
    {
        std::string text;
        if (read_file(store_.config_dir() + "/" + kPrefsFile, text, 64 * 1024)) {
            if (!prefs_from_json(text, prefs_)) {
                rename_path(store_.config_dir() + "/" + kPrefsFile, store_.config_dir() + "/" + kPrefsFile + ".bad");
                prefs_ = UiPrefs();
            }
        }
    }
    Game g;
    if (store_.load_game(g) == Store::Load::Ok && !g.won()) {
        game_ = std::move(g);
        recorded_ = false;
        return true;
    }
    game_.new_game(random_deal(), rules_);
    recorded_ = false;
    return false;
}

void Session::set_rules(const Rules &r)
{
    rules_ = r;
    store_.save_rules(rules_);
}

void Session::save_prefs()
{
    make_dirs(store_.config_dir());
    write_file_atomic(store_.config_dir() + "/" + kPrefsFile, prefs_to_json(prefs_));
}

void Session::reset_stats(const std::string &key)
{
    stats_.reset(key);
    store_.save_stats(stats_);
}

void Session::reset_all_stats()
{
    stats_.reset_all();
    store_.save_stats(stats_);
}

std::uint32_t Session::random_deal()
{
    /* xorshift64*: deal numbers of up to six digits are easy to read and to type */
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const std::uint64_t v = rng_ * 0x2545F4914F6CDD1Dull;
    return static_cast<std::uint32_t>((v >> 20) % 999999u) + 1u;
}

void Session::abandon_current()
{
    if (!in_progress()) return;
    GameRecord rec = game_.record();
    rec.won = false;
    stats_.record(game_.rules(), rec);
    store_.save_stats(stats_);
}

void Session::begin_deal(std::uint32_t deal, std::uint32_t user, bool winnable)
{
    abandon_current();
    game_.new_game(deal, rules_);
    game_.set_labels(user, winnable);
    recorded_ = false;
    save_game();
}

void Session::restart()
{
    abandon_current();
    const Board start = game_.start();
    const Rules rules = game_.rules();
    const std::uint32_t number = game_.deal_number();
    const std::uint32_t user = game_.user_number();
    const bool winnable = game_.winnable();
    game_.new_game(start, rules, number);
    game_.set_labels(user, winnable);
    recorded_ = false;
    save_game();
}

void Session::finish_if_won()
{
    if (recorded_ || !game_.won()) return;
    stats_.record(game_.rules(), game_.record());
    store_.save_stats(stats_);
    store_.clear_game();
    recorded_ = true;
}

bool Session::play(const Move &m, bool chained)
{
    if (recorded_ || !game_.apply(m, chained)) return false;
    finish_if_won();
    if (!recorded_) save_game();
    return true;
}

int Session::auto_moves()
{
    if (recorded_ || !game_.rules().auto_foundation) return 0;
    int n = 0;
    while (n < kCards) {
        const Move m = game_.safe_auto_move();
        if (m.count == 0 || !game_.apply(m, true)) break;
        ++n;
    }
    if (n) {
        finish_if_won();
        if (!recorded_) save_game();
    }
    return n;
}

bool Session::undo()
{
    if (recorded_ || !game_.undo()) return false;
    save_game();
    return true;
}

bool Session::redo()
{
    if (recorded_ || !game_.redo()) return false;
    save_game();
    return true;
}

void Session::tick(std::uint32_t ms)
{
    if (recorded_ || !game_.started()) return;
    game_.add_time(ms);
}

void Session::save_game()
{
    if (!recorded_) store_.save_game(game_);
}

std::int64_t Session::vegas_bank_now() const
{
    if (game_.rules().scoring != Scoring::Vegas) return 0;
    const std::int64_t bank = stats_.get(game_.rules().variation_key()).vegas_bank;
    return recorded_ ? bank : bank + game_.score();
}

/* ------------------------------------------------------------------------------------------ the solver helper */

std::string worker_winnable_reply(std::uint32_t user, const Rules &rules)
{
    const WinnableDeal d = find_winnable_deal(user, rules);
    char buf[96];
    if (!d.found) return "winnable 0";
    std::snprintf(buf, sizeof buf, "winnable 1 %u %u", static_cast<unsigned>(d.deal), static_cast<unsigned>(d.tries));
    return buf;
}

std::string worker_deadend_reply(const std::string &board_text, const Rules &rules)
{
    Board b;
    if (!Board::from_text(board_text, b)) return "deadend 0";
    Game g;
    if (!g.new_game(b, rules)) return "deadend 0";
    return g.dead_end() ? "deadend 1" : "deadend 0";
}

bool parse_worker_winnable(const std::string &reply, bool &found, std::uint32_t &deal)
{
    unsigned f = 0, d = 0, t = 0;
    if (std::sscanf(reply.c_str(), "winnable %u %u %u", &f, &d, &t) == 3 && f == 1) {
        found = true;
        deal = d;
        return true;
    }
    if (std::sscanf(reply.c_str(), "winnable %u", &f) == 1 && f == 0) {
        found = false;
        return true;
    }
    return false;
}

bool parse_worker_deadend(const std::string &reply, bool &dead)
{
    unsigned d = 0;
    if (std::sscanf(reply.c_str(), "deadend %u", &d) != 1) return false;
    dead = d != 0;
    return true;
}

} // namespace klondike
