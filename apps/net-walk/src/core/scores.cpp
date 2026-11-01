/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the scores file (see scores.hpp).
 */
#include "scores.hpp"

#include "fsutil.hpp"

#include <cstdlib>

namespace netwalk {

namespace {

const char *const kAppDir = "net-walk";

/* the integer after `"name":` at or after `from` and before `to`, or -1 */
long find_int(const std::string &s, const std::string &name, std::size_t from, std::size_t to)
{
    const std::string needle = "\"" + name + "\"";
    const std::size_t p = s.find(needle, from);
    if (p == std::string::npos || p >= to) return -1;
    std::size_t q = s.find(':', p + needle.size());
    if (q == std::string::npos || q >= to) return -1;
    ++q;
    while (q < to && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n' || s[q] == '\r')) ++q;
    char *end = nullptr;
    const long v = std::strtol(s.c_str() + q, &end, 10);
    if (end == s.c_str() + q) return -1;
    return v;
}

} // namespace

Record Scores::record(Level l, int moves, int seconds)
{
    Record r;
    Best &b = best_[static_cast<int>(l)];
    if (seconds < 1) seconds = 1;
    if (moves < 1) return r;
    if (b.moves == 0 || moves < b.moves) {
        b.moves = moves;
        r.new_moves = true;
    }
    if (b.seconds == 0 || seconds < b.seconds) {
        b.seconds = seconds;
        r.new_time = true;
    }
    return r;
}

void Scores::clear()
{
    for (Best &b : best_) b = Best();
    last_ = Level::Normal;
}

std::string Scores::to_json() const
{
    std::string s = std::string("{\"version\":1,\"last\":\"") + level_info(last_).key + "\",\"best\":{";
    for (int i = 0; i < kLevelCount; ++i) {
        if (i) s += ",";
        s += "\"";
        s += level_info(static_cast<Level>(i)).key;
        s += "\":{\"moves\":" + std::to_string(best_[i].moves) + ",\"seconds\":" + std::to_string(best_[i].seconds) + "}";
    }
    s += "}}\n";
    return s;
}

bool Scores::from_json(const std::string &text, Scores &out)
{
    out.clear();
    const std::size_t root = text.find("\"best\"");
    if (text.find('{') == std::string::npos || root == std::string::npos) return false;
    const std::size_t lp = text.find("\"last\"");
    if (lp != std::string::npos && lp < root) {
        const std::size_t q1 = text.find('"', lp + 6 + 1);
        const std::size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
        if (q2 != std::string::npos) out.last_ = level_from_key(text.substr(q1 + 1, q2 - q1 - 1));
    }
    for (int i = 0; i < kLevelCount; ++i) {
        const std::string key = std::string("\"") + level_info(static_cast<Level>(i)).key + "\"";
        const std::size_t p = text.find(key, root);
        if (p == std::string::npos) continue;
        const std::size_t open = text.find('{', p);
        const std::size_t close = open == std::string::npos ? open : text.find('}', open);
        if (close == std::string::npos) continue;
        const long m = find_int(text, "moves", open, close);
        const long t = find_int(text, "seconds", open, close);
        if (m > 0 && m < 1000000) out.best_[i].moves = static_cast<int>(m);
        if (t > 0 && t < 100000000) out.best_[i].seconds = static_cast<int>(t);
    }
    return true;
}

std::string ScoreStore::default_dir()
{
    if (const char *x = std::getenv("NETWALK_DATA"); x && *x) return x;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/" + kAppDir;
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : "/tmp") + "/.local/share/" + kAppDir;
}

ScoreStore::ScoreStore() : dir_(default_dir()) {}

Scores ScoreStore::load() const
{
    Scores s;
    std::string text;
    if (!read_file(dir_ + "/scores.json", text)) return s;
    if (!Scores::from_json(text, s)) s.clear();
    return s;
}

bool ScoreStore::save(const Scores &s) const
{
    return make_dirs(dir_) && write_file_atomic(dir_ + "/scores.json", s.to_json());
}

} // namespace netwalk
