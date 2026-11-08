/*
 * SPDX-License-Identifier: MIT
 */
#include "naming.hpp"

#include "../journal.hpp"
#include "atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <set>

namespace cassette {
namespace {

bool all_digits(const std::string &s, size_t from, size_t n) {
    if (from + n > s.size() || n == 0) return false;
    for (size_t k = from; k < from + n; k++)
        if (s[k] < '0' || s[k] > '9') return false;
    return true;
}

int to_int(const std::string &s, size_t from, size_t n) {
    int v = 0;
    for (size_t k = from; k < from + n; k++) v = v * 10 + (s[k] - '0');
    return v;
}

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

/* A trailing "<sep><digits>" of at most 9 digits, no leading zero. Sets value and the length removed. */
bool trailing_number(const std::string &s, const std::string &sep, uint32_t &value, size_t &cut) {
    size_t k = s.size();
    while (k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9') k--;
    const size_t nd = s.size() - k;
    if (nd == 0 || nd > 9 || s[k] == '0') return false;
    if (k < sep.size() || s.compare(k - sep.size(), sep.size(), sep) != 0) return false;
    value = static_cast<uint32_t>(std::stoul(s.substr(k)));
    cut = nd + sep.size();
    return true;
}

/* <prefix>_<take>_<when> */
bool parse_base(const std::string &s, TakeName &t) {
    std::string rest;
    if (ends_with(s, "_nodate")) {
        t.has_date = false;
        t.when = DateTime{};
        rest = s.substr(0, s.size() - 7);
    } else {
        if (s.size() < 14) return false;
        const size_t w = s.size() - 14;
        if (s[w] != '_' || s[w + 9] != '-' || !all_digits(s, w + 1, 8) || !all_digits(s, w + 10, 4)) return false;
        DateTime d;
        d.year = to_int(s, w + 1, 4);
        d.month = to_int(s, w + 5, 2);
        d.day = to_int(s, w + 7, 2);
        d.hour = to_int(s, w + 10, 2);
        d.minute = to_int(s, w + 12, 2);
        if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31 || d.hour > 23 || d.minute > 59) return false;
        t.has_date = true;
        t.when = d;
        rest = s.substr(0, w);
    }
    const size_t u = rest.find_last_of('_');
    if (u == std::string::npos) return false;
    const std::string num = rest.substr(u + 1);
    if (num.size() < 3 || num.size() > 18 || !all_digits(num, 0, num.size())) return false;
    if (num.size() > 3 && num[0] == '0') return false; /* not the canonical padding */
    t.take = std::stoull(num);
    t.prefix = rest.substr(0, u);
    return valid_prefix(t.prefix);
}

/* base [_p<part>] */
bool parse_core(const std::string &s, TakeName &t) {
    uint32_t part = 0;
    size_t cut = 0;
    if (trailing_number(s, "_p", part, cut) && part >= 2 && parse_base(s.substr(0, s.size() - cut), t)) {
        t.part = part;
        return true;
    }
    t.part = 1;
    return parse_base(s, t);
}

std::string key_sanitize(const std::string &s) {
    std::string r;
    for (unsigned char c : s) r += (c == '/' || c < 0x20) ? '_' : static_cast<char>(c);
    return r;
}

void collect_take(FileOps &, const std::string &name, uint64_t &best) {
    TakeName t;
    if (parse_take_name(base_name(name), t) && t.take > best) best = t.take;
}

}  // namespace

bool datetime_from_wall(int64_t wall_s, DateTime &out, bool utc) {
    std::time_t tt = static_cast<std::time_t>(wall_s);
    std::tm tm{};
    if (!(utc ? gmtime_r(&tt, &tm) : localtime_r(&tt, &tm))) return false;
    out.year = tm.tm_year + 1900;
    out.month = tm.tm_mon + 1;
    out.day = tm.tm_mday;
    out.hour = tm.tm_hour;
    out.minute = tm.tm_min;
    return true;
}

bool TimesyncClockState::synchronized() { return path_exists(ops_, marker_); }

bool valid_prefix(const std::string &p) {
    if (p.empty() || p.size() > kMaxPrefix || p[0] == '.') return false;
    for (char c : p) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string take_number_text(uint64_t take) {
    char b[32];
    std::snprintf(b, sizeof b, "%03llu", static_cast<unsigned long long>(take));
    return b;
}

std::string format_take_name(const TakeName &t) {
    std::string s = t.prefix + "_" + take_number_text(t.take) + "_";
    if (t.has_date) {
        char b[32];
        std::snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d", t.when.year % 10000, t.when.month % 100, t.when.day % 100,
                      t.when.hour % 100, t.when.minute % 100);
        s += b;
    } else {
        s += "nodate";
    }
    if (t.part >= 2) s += "_p" + std::to_string(t.part);
    if (t.edit >= 1) s += "-" + std::to_string(t.edit);
    return s + ".wav";
}

bool has_wav_extension(const std::string &n) { return ends_with(n, ".wav") || ends_with(n, ".WAV"); }

std::string file_stem(const std::string &n) { return has_wav_extension(n) ? n.substr(0, n.size() - 4) : n; }

bool parse_take_name(const std::string &file_name, TakeName &out) {
    if (!ends_with(file_name, ".wav")) return false;
    const std::string s = file_name.substr(0, file_name.size() - 4);
    TakeName t;
    uint32_t edit = 0;
    size_t cut = 0;
    if (trailing_number(s, "-", edit, cut) && parse_core(s.substr(0, s.size() - cut), t)) {
        t.edit = edit;
        out = t;
        return true;
    }
    t = TakeName{};
    if (!parse_core(s, t)) return false;
    t.edit = 0;
    out = t;
    return true;
}

std::string take_label(const TakeName &t) {
    std::string s = take_number_text(t.take);
    if (t.part >= 2) s += " p" + std::to_string(t.part);
    if (t.edit >= 1) s += "-" + std::to_string(t.edit);
    return s;
}

TakeName new_take_name(const std::string &prefix, uint64_t take, ClockState &cs, Clock &clock) {
    TakeName t;
    t.prefix = prefix;
    t.take = take;
    t.has_date = cs.synchronized() && datetime_from_wall(clock.wall_s(), t.when);
    if (!t.has_date) t.when = DateTime{};
    return t;
}

/* ---- take counter ---- */

std::string take_counter_path(const std::string &dir) { return dir + "/.take-counter"; }

uint64_t highest_take_found(FileOps &ops, const std::string &dir) {
    uint64_t best = 0;
    std::vector<std::string> names;
    if (ops.list_dir(dir, names) == 0)
        for (const std::string &n : names) collect_take(ops, n, best);
    std::vector<std::string> jnames;
    const std::string jd = journal_dir(dir);
    if (ops.list_dir(jd, jnames) == 0) {
        for (const std::string &n : jnames) {
            if (!ends_with(n, ".json")) continue;
            JournalEntry e;
            if (journal_read(ops, jd + "/" + n, e) != 0) continue;
            collect_take(ops, e.take + ".wav", best);
            collect_take(ops, e.path, best);
            for (const std::string &p : e.parts) collect_take(ops, p, best);
        }
    }
    return best;
}

int reserve_take_number(FileOps &ops, const std::string &dir, uint64_t &take) {
    int r = make_dirs(ops, dir);
    if (r < 0) return r;
    uint64_t last = 0;
    std::string body;
    if (read_small_file(ops, take_counter_path(dir), body, 64) == 0) {
        unsigned long long v = 0;
        if (std::sscanf(body.c_str(), "%llu", &v) == 1) last = v;
    }
    last = std::max<uint64_t>(last, highest_take_found(ops, dir));
    const uint64_t next = last + 1;
    r = atomic_write_file(ops, take_counter_path(dir), std::to_string(next) + "\n");
    if (r < 0) return r;
    take = next;
    return 0;
}

/* ---- edit suffixes ---- */

std::string edit_record_key(const std::string &source_name) {
    TakeName t;
    if (parse_take_name(base_name(source_name), t)) {
        std::string k = take_number_text(t.take);
        if (t.part >= 2) k += "_p" + std::to_string(t.part);
        return k;
    }
    return "x_" + key_sanitize(file_stem(base_name(source_name)));
}

std::string edit_record_path(const std::string &dir, const std::string &source_name) {
    return dir + "/.edits/" + edit_record_key(source_name) + ".used";
}

std::vector<std::string> next_edit_names(const std::string &source_name, unsigned count,
                                         const std::vector<std::string> &existing, const std::vector<uint32_t> &used) {
    std::vector<std::string> out;
    if (count == 0) return out;
    const std::string src = base_name(source_name);
    uint32_t top = 0;
    for (uint32_t u : used) top = std::max(top, u);
    TakeName t;
    if (parse_take_name(src, t)) {
        for (const std::string &n : existing) {
            TakeName e;
            if (parse_take_name(base_name(n), e) && e.take == t.take && e.part == t.part) top = std::max(top, e.edit);
        }
        top = std::max(top, t.edit);
        for (unsigned k = 1; k <= count; k++) {
            TakeName r = t;
            r.edit = top + k;
            out.push_back(format_take_name(r));
        }
        return out;
    }
    const std::string stem = file_stem(src);
    for (const std::string &n : existing) {
        const std::string b = file_stem(base_name(n));
        uint32_t v = 0;
        size_t cut = 0;
        if (b.size() > stem.size() && b.compare(0, stem.size(), stem) == 0 && trailing_number(b, "-", v, cut) &&
            b.size() - cut == stem.size())
            top = std::max(top, v);
    }
    for (unsigned k = 1; k <= count; k++) out.push_back(stem + "-" + std::to_string(top + k) + ".wav");
    return out;
}

std::vector<uint32_t> read_edit_record(FileOps &ops, const std::string &dir, const std::string &source_name) {
    std::vector<uint32_t> v;
    std::string body;
    if (read_small_file(ops, edit_record_path(dir, source_name), body, 64 * 1024) != 0) return v;
    size_t p = 0;
    while (p < body.size()) {
        size_t e = body.find('\n', p);
        if (e == std::string::npos) e = body.size();
        const std::string line = body.substr(p, e - p);
        if (!line.empty() && line.size() <= 9 && all_digits(line, 0, line.size()))
            v.push_back(static_cast<uint32_t>(std::stoul(line)));
        p = e + 1;
    }
    return v;
}

int reserve_edit_names(FileOps &ops, const std::string &dir, const std::string &source_name, unsigned count,
                       std::vector<std::string> &out) {
    out.clear();
    if (count == 0) return 0;
    std::vector<std::string> names;
    int r = ops.list_dir(dir, names);
    if (r < 0) return r;
    std::vector<uint32_t> used = read_edit_record(ops, dir, source_name);
    std::vector<std::string> picked = next_edit_names(source_name, count, names, used);
    std::set<uint32_t> all(used.begin(), used.end());
    for (const std::string &n : picked) {
        uint32_t v = 0;
        size_t cut = 0;
        if (trailing_number(file_stem(n), "-", v, cut)) all.insert(v);
    }
    std::string body;
    for (uint32_t v : all) body += std::to_string(v) + "\n";
    r = make_dirs(ops, dir + "/.edits");
    if (r == 0) r = atomic_write_file(ops, edit_record_path(dir, source_name), body);
    if (r < 0) return r;
    out = picked;
    return 0;
}

}  // namespace cassette
