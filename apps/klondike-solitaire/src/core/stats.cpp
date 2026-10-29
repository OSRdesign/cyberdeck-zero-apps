/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the statistics (see stats.hpp).
 */
#include "stats.hpp"

#include "json.hpp"

namespace klondike {

constexpr int kStatsVersion = 1;

bool VariationStats::operator==(const VariationStats &o) const
{
    return played == o.played && won == o.won && has_best_score == o.has_best_score &&
           (!has_best_score || best_score == o.best_score) && best_time_s == o.best_time_s &&
           fewest_moves == o.fewest_moves && streak == o.streak && best_streak == o.best_streak &&
           total_time_s == o.total_time_s && vegas_bank == o.vegas_bank;
}

void Stats::record(const Rules &rules, const GameRecord &game)
{
    VariationStats &v = by_key_[rules.variation_key()];
    ++v.played;
    v.total_time_s += game.seconds;
    if (rules.scoring != Scoring::None && (!v.has_best_score || game.score > v.best_score)) {
        v.has_best_score = true;
        v.best_score = game.score;
    }
    if (rules.scoring == Scoring::Vegas) v.vegas_bank += game.score;
    if (game.won) {
        ++v.won;
        ++v.streak;
        if (v.streak > v.best_streak) v.best_streak = v.streak;
        const std::uint32_t t = game.seconds > 0 ? game.seconds : 1; /* 0 means "none": a win under 1 s counts 1 s */
        if (v.best_time_s == 0 || t < v.best_time_s) v.best_time_s = t;
        if (v.fewest_moves == 0 || game.moves < v.fewest_moves) v.fewest_moves = game.moves;
    } else {
        v.streak = 0;
    }
}

VariationStats Stats::get(const std::string &key) const
{
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? VariationStats() : it->second;
}

std::string Stats::to_json() const
{
    std::string s = "{\n  \"version\": " + std::to_string(kStatsVersion) + ",\n  \"variations\": {";
    bool first = true;
    for (const auto &kv : by_key_) {
        const VariationStats &v = kv.second;
        s += first ? "\n" : ",\n";
        first = false;
        s += "    " + json::quote(kv.first) + ": {";
        s += "\"played\": " + std::to_string(v.played);
        s += ", \"won\": " + std::to_string(v.won);
        if (v.has_best_score) s += ", \"best_score\": " + std::to_string(v.best_score);
        s += ", \"best_time\": " + std::to_string(v.best_time_s);
        s += ", \"fewest_moves\": " + std::to_string(v.fewest_moves);
        s += ", \"streak\": " + std::to_string(v.streak);
        s += ", \"best_streak\": " + std::to_string(v.best_streak);
        s += ", \"total_time\": " + std::to_string(v.total_time_s);
        s += ", \"bank\": " + std::to_string(v.vegas_bank);
        s += "}";
    }
    s += first ? "}\n}\n" : "\n  }\n}\n";
    return s;
}

bool Stats::from_json(const std::string &text, Stats &out, std::string *why)
{
    auto fail = [why](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    json::Value root;
    std::string err;
    if (!json::parse(text, root, &err)) return fail("not JSON: " + err);
    const json::Value *ver = root.get("version");
    if (!ver || !ver->is_int()) return fail("no version");
    if (ver->as_int() > kStatsVersion) return fail("written by a newer version");
    if (ver->as_int() < 1) return fail("bad version");
    const json::Value *vars = root.get("variations");
    if (!vars || vars->kind != json::Value::Kind::Object) return fail("no variations");
    Stats st;
    for (const auto &kv : vars->members) {
        const json::Value &o = kv.second;
        if (o.kind != json::Value::Kind::Object || kv.first.empty() || kv.first.size() > 64)
            return fail("bad variation '" + kv.first + "'");
        VariationStats v;
        auto u32 = [&o](const char *key, std::uint32_t &dst) {
            const json::Value *x = o.get(key);
            if (!x) return true; /* missing: 0 */
            if (!x->is_int() || x->as_int() < 0 || x->as_int() > 0xFFFFFFFFll) return false;
            dst = static_cast<std::uint32_t>(x->as_int());
            return true;
        };
        if (!u32("played", v.played) || !u32("won", v.won) || !u32("best_time", v.best_time_s) ||
            !u32("fewest_moves", v.fewest_moves) || !u32("streak", v.streak) || !u32("best_streak", v.best_streak))
            return fail("bad numbers in '" + kv.first + "'");
        if (v.won > v.played || v.streak > v.won || v.best_streak > v.won || v.streak > v.best_streak)
            return fail("inconsistent counts in '" + kv.first + "'");
        if (const json::Value *x = o.get("best_score")) {
            if (!x->is_int() || x->as_int() < -0x7FFFFFFFll || x->as_int() > 0x7FFFFFFFll)
                return fail("bad best score in '" + kv.first + "'");
            v.has_best_score = true;
            v.best_score = static_cast<std::int32_t>(x->as_int());
        }
        if (const json::Value *x = o.get("total_time")) {
            if (!x->is_int() || x->as_int() < 0) return fail("bad total time in '" + kv.first + "'");
            v.total_time_s = static_cast<std::uint64_t>(x->as_int());
        }
        if (const json::Value *x = o.get("bank")) {
            if (!x->is_int()) return fail("bad bank in '" + kv.first + "'");
            v.vegas_bank = x->as_int();
        }
        st.by_key_[kv.first] = v;
    }
    out = std::move(st);
    return true;
}

} // namespace klondike
