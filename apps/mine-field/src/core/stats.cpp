/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the statistics (see stats.hpp).
 */
#include "stats.hpp"

#include "json.hpp"

#include <algorithm>

namespace minefield {

constexpr int kStatsVersion = 2;   /* 1 (fixed boards, no sizes in the best list) is not read: the store sets it aside */

bool PresetStats::operator==(const PresetStats &o) const
{
    return played == o.played && won == o.won && streak == o.streak && best_streak == o.best_streak &&
           total_time_ms == o.total_time_ms && best == o.best;
}

bool Stats::same(const Stats &o) const
{
    return by_ == o.by_;
}

bool Stats::record(const GameRecord &g)
{
    if (!g.started) return false;
    PresetStats &s = by_[static_cast<std::size_t>(g.preset)];
    ++s.played;
    s.total_time_ms += g.time_ms;
    if (g.won) {
        ++s.won;
        ++s.streak;
        s.best_streak = std::max(s.best_streak, s.streak);
        if (!g.assisted) {
            BestEntry e;
            e.time_ms = g.time_ms;
            e.seed = g.seed;
            e.fair = g.fair;
            e.w = g.dims.w;
            e.h = g.dims.h;
            e.mines = g.dims.mines;
            /* an equal time ranks after the older entries */
            auto it = std::upper_bound(s.best.begin(), s.best.end(), e,
                                       [](const BestEntry &a, const BestEntry &b) { return a.time_ms < b.time_ms; });
            s.best.insert(it, e);
            if (s.best.size() > static_cast<std::size_t>(kBestCount)) s.best.resize(kBestCount);
        }
    } else {
        s.streak = 0;
    }
    return true;
}

PresetStats Stats::get(Preset p) const
{
    return by_[static_cast<std::size_t>(p)];
}

int Stats::rank_of(Preset p, std::uint32_t time_ms) const
{
    const std::vector<BestEntry> &b = by_[static_cast<std::size_t>(p)].best;
    int rank = 0;
    while (rank < static_cast<int>(b.size()) && b[static_cast<std::size_t>(rank)].time_ms <= time_ms) ++rank;
    return rank < kBestCount ? rank : -1;
}

void Stats::reset(Preset p)
{
    by_[static_cast<std::size_t>(p)] = PresetStats();
}

void Stats::reset_all()
{
    by_ = {};
}

std::string Stats::to_json() const
{
    std::string s = "{\n  \"version\": " + std::to_string(kStatsVersion) + ",\n  \"presets\": {";
    for (int p = 0; p < kRecordedPresets; ++p) {
        const PresetStats &t = by_[static_cast<std::size_t>(p)];
        s += p ? ",\n    " : "\n    ";
        s += json::quote(preset_name(static_cast<Preset>(p))) + ": {\"played\": " + std::to_string(t.played) +
             ", \"won\": " + std::to_string(t.won) + ", \"streak\": " + std::to_string(t.streak) +
             ", \"best_streak\": " + std::to_string(t.best_streak) + ", \"total_time_ms\": " + std::to_string(t.total_time_ms) +
             ", \"best\": [";
        for (std::size_t k = 0; k < t.best.size(); ++k) {
            if (k) s += ", ";
            s += "{\"time_ms\": " + std::to_string(t.best[k].time_ms) + ", \"seed\": \"" + std::to_string(t.best[k].seed) +
                 "\", \"fair\": " + (t.best[k].fair ? "true" : "false") + ", \"w\": " + std::to_string(t.best[k].w) +
                 ", \"h\": " + std::to_string(t.best[k].h) + ", \"mines\": " + std::to_string(t.best[k].mines) + "}";
        }
        s += "]}";
    }
    s += "\n  }\n}\n";
    return s;
}

static bool read_u64_string(const json::Value &v, std::uint64_t &out)
{
    if (v.kind != json::Value::Kind::String || v.text.empty() || v.text.size() > 20) return false;
    std::uint64_t r = 0;
    for (char c : v.text) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (r > (0xFFFFFFFFFFFFFFFFull - d) / 10) return false;
        r = r * 10 + d;
    }
    out = r;
    return true;
}

static bool read_count(const json::Value &obj, const char *key, std::uint64_t max, std::uint64_t &out)
{
    const json::Value *v = obj.get(key);
    if (!v || !v->is_int() || v->as_int() < 0 || static_cast<std::uint64_t>(v->as_int()) > max) return false;
    out = static_cast<std::uint64_t>(v->as_int());
    return true;
}

bool Stats::from_json(const std::string &text, Stats &out, std::string *why)
{
    const auto fail = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    json::Value root;
    std::string jwhy;
    if (!json::parse(text, root, &jwhy)) return fail("not JSON");
    if (root.kind != json::Value::Kind::Object) return fail("not an object");
    const json::Value *ver = root.get("version");
    if (!ver || !ver->is_int() || ver->as_int() != kStatsVersion) return fail("bad version");
    const json::Value *presets = root.get("presets");
    if (!presets || presets->kind != json::Value::Kind::Object) return fail("no presets");
    Stats r;
    for (const auto &m : presets->members) {
        Preset p;
        if (!parse_preset(m.first, p)) continue;   /* unknown names are skipped */
        const json::Value &o = m.second;
        if (o.kind != json::Value::Kind::Object) return fail("bad preset entry");
        PresetStats &t = r.by_[static_cast<std::size_t>(p)];
        std::uint64_t v;
        if (!read_count(o, "played", 0xFFFFFFFFull, v)) return fail("bad played");
        t.played = static_cast<std::uint32_t>(v);
        if (!read_count(o, "won", 0xFFFFFFFFull, v) || v > t.played) return fail("bad won");
        t.won = static_cast<std::uint32_t>(v);
        if (!read_count(o, "streak", 0xFFFFFFFFull, v)) return fail("bad streak");
        t.streak = static_cast<std::uint32_t>(v);
        if (!read_count(o, "best_streak", 0xFFFFFFFFull, v) || v < t.streak) return fail("bad best streak");
        t.best_streak = static_cast<std::uint32_t>(v);
        if (!read_count(o, "total_time_ms", 0x7FFFFFFFFFFFFFFFull, v)) return fail("bad total time");
        t.total_time_ms = v;
        const json::Value *best = o.get("best");
        if (!best || best->kind != json::Value::Kind::Array) return fail("bad best list");
        for (const json::Value &b : best->items) {
            BestEntry e;
            if (b.kind != json::Value::Kind::Object) return fail("bad best entry");
            if (!read_count(b, "time_ms", 0xFFFFFFFFull, v)) return fail("bad best time");
            e.time_ms = static_cast<std::uint32_t>(v);
            const json::Value *sd = b.get("seed"), *fr = b.get("fair");
            if (!sd || !read_u64_string(*sd, e.seed)) return fail("bad best seed");
            if (!fr || fr->kind != json::Value::Kind::Bool) return fail("bad best flag");
            e.fair = fr->boolean;
            if (!read_count(b, "w", kMaxDim, v) || v < kMinDim) return fail("bad best width");
            e.w = static_cast<int>(v);
            if (!read_count(b, "h", kMaxDim, v) || v < kMinDim) return fail("bad best height");
            e.h = static_cast<int>(v);
            if (!read_count(b, "mines", kMaxCells, v) || v < 1) return fail("bad best mines");
            e.mines = static_cast<int>(v);
            t.best.push_back(e);
        }
        std::stable_sort(t.best.begin(), t.best.end(),
                         [](const BestEntry &a, const BestEntry &b) { return a.time_ms < b.time_ms; });
        if (t.best.size() > static_cast<std::size_t>(kBestCount)) t.best.resize(kBestCount);
    }
    out = r;
    return true;
}

} // namespace minefield
