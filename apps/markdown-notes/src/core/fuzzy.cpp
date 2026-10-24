/*
 * SPDX-License-Identifier: MIT
 */
#include "fuzzy.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <cstdint>

namespace mdnotes {

namespace {

bool word_start(const std::string &t, std::size_t i)
{
    if (i == 0) return true;
    const char p = t[i - 1];
    return p == ' ' || p == '-' || p == '_' || p == '.' || p == '/' || p == '(' || p == '[';
}

std::vector<std::string> words_of(const std::string &q)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : q) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

std::string trim_spaces(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

} // namespace

int fuzzy_word_score(const std::string &word, const std::string &text)
{
    if (word.empty()) return 0;
    if (word.size() > text.size()) return -1;
    /* a substring: the best place (a word start first, then the earliest) */
    int best = -1;
    for (std::size_t p = text.find(word); p != std::string::npos; p = text.find(word, p + 1)) {
        int s = 100 + static_cast<int>(word.size()) * 4;
        if (p == 0) s += 60;
        else if (word_start(text, p)) s += 40;
        s -= static_cast<int>(std::min<std::size_t>(p, 30));
        best = std::max(best, s);
    }
    if (best >= 0) return best;
    /* a subsequence of code points, greedy from the left, preferring a word start for the next letter when one is
     * ahead (a run of consecutive letters beats a later word start) */
    std::vector<std::uint32_t> w, t;
    for (std::size_t i = 0; i < word.size();) w.push_back(utf8_next(word, i));
    for (std::size_t i = 0; i < text.size();) t.push_back(utf8_next(text, i));
    const auto starts = [&](std::size_t k) {
        if (k == 0) return true;
        const std::uint32_t p = t[k - 1];
        return p == ' ' || p == '-' || p == '_' || p == '.' || p == '/' || p == '(' || p == '[';
    };
    int s = 0;
    std::size_t ti = 0, prev = std::string::npos;
    for (std::size_t wi = 0; wi < w.size(); ++wi) {
        std::size_t hit = std::string::npos, ws = std::string::npos;
        for (std::size_t k = ti; k < t.size(); ++k) {
            if (t[k] != w[wi]) continue;
            if (hit == std::string::npos) hit = k;
            if (starts(k)) {
                ws = k;
                break;
            }
            if (prev != std::string::npos && k == prev + 1) break;
        }
        if (hit == std::string::npos) return -1;
        std::size_t at = hit;
        if (ws != std::string::npos && !(prev != std::string::npos && hit == prev + 1)) at = ws;
        s += 10;
        if (starts(at)) s += 15;
        if (prev != std::string::npos && at == prev + 1) s += 8;
        else if (prev != std::string::npos) s -= static_cast<int>(std::min<std::size_t>(at - prev - 1, 10));
        prev = at;
        ti = at + 1;
    }
    return std::max(s, 1);
}

std::vector<Match> rank_notes(const VaultIndex &ix, const std::string &query, std::size_t limit)
{
    std::vector<Match> out;
    const std::string q = fold_case(trim_spaces(query));
    const std::vector<std::string> words = words_of(q);
    if (words.empty()) return out;
    for (std::size_t i = 0; i < ix.notes.size(); ++i) {
        const NoteEntry &n = ix.notes[i];
        const std::string folder = fold_case(n.folder);
        int total = 0;
        bool all = true;
        for (const std::string &w : words) {
            int s = fuzzy_word_score(w, n.folded);
            if (s < 0) {
                const int f = folder.empty() ? -1 : fuzzy_word_score(w, folder);
                if (f < 0) {
                    all = false;
                    break;
                }
                s = std::max(1, f / 3);
            }
            total += s;
        }
        if (!all) continue;
        Match m;
        m.note = static_cast<int>(i);
        m.exact = n.folded == q;
        m.score = total + (m.exact ? 1000 : 0);
        out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [&](const Match &a, const Match &b) {
        if (a.score != b.score) return a.score > b.score;
        const NoteEntry &x = ix.notes[static_cast<std::size_t>(a.note)], &y = ix.notes[static_cast<std::size_t>(b.note)];
        if (x.name.size() != y.name.size()) return x.name.size() < y.name.size();
        if (x.mtime_ns != y.mtime_ns) return x.mtime_ns > y.mtime_ns;
        return x.rel < y.rel;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

bool has_exact(const std::vector<Match> &matches)
{
    for (const Match &m : matches)
        if (m.exact) return true;
    return false;
}

} // namespace mdnotes
