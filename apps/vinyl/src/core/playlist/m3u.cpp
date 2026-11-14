/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl playlists: M3U / M3U8 reading and writing (see m3u.hpp).
 */
#include "m3u.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>

namespace vinyl {
namespace m3u {

namespace {

bool real_file_exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void put_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/* Windows-1252 for 0x80-0x9F (the undefined ones stay the C1 controls); every other byte is Latin-1 */
uint32_t cp1252(unsigned char b) {
    static const uint16_t hi[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    return b >= 0x80 && b <= 0x9F ? hi[b - 0x80] : b;
}

std::string latin1_to_utf8(const std::string &s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) put_utf8(out, cp1252(c));
    return out;
}

std::string utf16_to_utf8(const std::string &s, bool little) {
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const unsigned a = static_cast<unsigned char>(s[i]), b = static_cast<unsigned char>(s[i + 1]);
        uint32_t u = little ? (a | (b << 8)) : ((a << 8) | b);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            const unsigned c = static_cast<unsigned char>(s[i + 2]), d = static_cast<unsigned char>(s[i + 3]);
            const uint32_t lo = little ? (c | (d << 8)) : ((c << 8) | d);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(out, u);
    }
    return out;
}

std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n' || c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool ieq_prefix(const std::string &s, const char *p) {
    size_t i = 0;
    for (; p[i]; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != p[i]) return false;
    return true;
}

std::string percent_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> split_path(const std::string &p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

struct Resolved {
    bool skip = false;      /* a URL that is not a file */
    std::string path;
    bool resolved = true;
    bool found = false;     /* the file is there */
};

/* One reading of a path line (`line` already trimmed, one spelling of its bytes). */
Resolved resolve_line(const std::string &line_in, const ParseOptions &opt, const std::function<bool(const std::string &)> &exists) {
    Resolved r;
    std::string line = line_in;
    if (ieq_prefix(line, "file://")) {
        line.erase(0, 7);
        if (ieq_prefix(line, "localhost/")) line.erase(0, 9);
        line = percent_decode(line);
        if (line.size() >= 3 && line[0] == '/' && std::isalpha(static_cast<unsigned char>(line[1])) && line[2] == ':') line.erase(0, 1);
    } else if (line.find("://") != std::string::npos) {
        r.skip = true; /* http, https, rtsp, mms ...: not a file */
        return r;
    }
    const bool unc = line.size() >= 2 && line[0] == '\\' && line[1] == '\\';
    std::string slashed = line;
    for (char &c : slashed)
        if (c == '\\') c = '/';
    const bool drive = slashed.size() >= 2 && std::isalpha(static_cast<unsigned char>(slashed[0])) && slashed[1] == ':';
    if (drive || unc) {
        /* a Windows path: drop leading folders until it is found under the playlist's folder or a known root */
        std::string rest = drive ? slashed.substr(2) : slashed;
        std::vector<std::string> parts = split_path(rest);
        if (unc && parts.size() >= 2) parts.erase(parts.begin(), parts.begin() + 2); /* server and share */
        std::vector<std::string> bases;
        if (!opt.base_dir.empty()) bases.push_back(opt.base_dir);
        for (const std::string &root : opt.roots)
            if (std::find(bases.begin(), bases.end(), root) == bases.end()) bases.push_back(root);
        for (size_t k = 0; k < parts.size(); ++k) {
            std::string tail;
            for (size_t j = k; j < parts.size(); ++j) tail += (tail.empty() ? "" : "/") + parts[j];
            for (const std::string &b : bases) {
                const std::string cand = normalize(b + "/" + tail);
                if (exists(cand)) {
                    r.path = cand;
                    r.found = true;
                    return r;
                }
            }
        }
        r.path = line; /* kept as it was read: a missing entry, drawn dimmed */
        r.resolved = false;
        return r;
    }
    const std::string cand = slashed[0] == '/' ? normalize(slashed) : normalize((opt.base_dir.empty() ? std::string("/") : opt.base_dir) + "/" + slashed);
    r.path = cand;
    r.found = exists(cand);
    return r;
}

/* "#EXTINF:123,Artist - Title" or "#EXTINF:123 key=\"v\",Title" */
bool parse_extinf(const std::string &line, int &seconds, std::string &text) {
    if (!ieq_prefix(line, "#extinf:")) return false;
    size_t i = 8;
    bool neg = false;
    if (i < line.size() && line[i] == '-') {
        neg = true;
        ++i;
    }
    long v = 0;
    bool digits = false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = std::min(v * 10 + (line[i] - '0'), 10000000L);
        ++i;
        digits = true;
    }
    seconds = digits && !neg ? static_cast<int>(v) : -1;
    const size_t comma = line.find(',', i);
    text = comma == std::string::npos ? std::string() : trim(line.substr(comma + 1));
    return true;
}

}  // namespace

bool valid_utf8(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t n = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
            return false;
        i += n + 1;
    }
    return true;
}

std::string decode_text(const std::string &bytes, bool *was_latin1) {
    if (was_latin1) *was_latin1 = false;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
        return utf16_to_utf8(bytes.substr(2), true);
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF)
        return utf16_to_utf8(bytes.substr(2), false);
    std::string s = bytes;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    if (valid_utf8(s)) return s;
    if (was_latin1) *was_latin1 = true;
    return latin1_to_utf8(s);
}

std::string normalize(const std::string &path) {
    const bool abs = !path.empty() && path[0] == '/';
    std::vector<std::string> out;
    for (const std::string &p : split_path(path)) {
        if (p == ".") continue;
        if (p == "..") {
            if (!out.empty() && out.back() != "..") out.pop_back();
            else if (!abs) out.push_back(p);
            continue;
        }
        out.push_back(p);
    }
    std::string r = abs ? "/" : "";
    for (size_t i = 0; i < out.size(); ++i) r += (i ? "/" : "") + out[i];
    return r;
}

std::string dir_of(const std::string &path) {
    const size_t s = path.rfind('/');
    if (s == std::string::npos) return std::string();
    return s == 0 ? std::string("/") : path.substr(0, s);
}

std::string relative_to(const std::string &dir, const std::string &path) {
    const std::vector<std::string> d = split_path(dir), p = split_path(path);
    size_t c = 0;
    while (c < d.size() && c < p.size() && d[c] == p[c]) ++c;
    if (c == 0 || p.empty()) return std::string();
    std::string r;
    for (size_t i = c; i < d.size(); ++i) r += "../";
    for (size_t i = c; i < p.size(); ++i) r += p[i] + (i + 1 < p.size() ? "/" : "");
    return r;
}

std::vector<Entry> parse(const std::string &bytes, const ParseOptions &opt) {
    std::vector<Entry> out;
    bool latin1 = false;
    const std::string text = decode_text(bytes, &latin1);
    const std::vector<std::string> lines = split_lines(text);
    /* the raw bytes' lines, to look a Latin-1 name up as it may lie on the disk (a file system that kept the old bytes) */
    std::vector<std::string> raw_lines;
    if (latin1) {
        std::string raw = bytes;
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB) raw.erase(0, 3);
        raw_lines = split_lines(raw);
    }
    const std::function<bool(const std::string &)> exists = opt.exists ? opt.exists : std::function<bool(const std::string &)>(real_file_exists);
    int pending_seconds = -1;
    std::string pending_text;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string line = trim(lines[li]);
        if (line.empty()) continue;
        if (line[0] == '#') {
            int sec = -1;
            std::string t;
            if (parse_extinf(line, sec, t)) {
                pending_seconds = sec;
                pending_text = t;
            }
            continue;
        }
        Resolved r = resolve_line(line, opt, exists);
        if (r.skip) {
            pending_seconds = -1;
            pending_text.clear();
            continue;
        }
        if (!r.found && latin1 && li < raw_lines.size()) {
            const Resolved rr = resolve_line(trim(raw_lines[li]), opt, exists);
            if (rr.found) r = rr;
        }
        Entry e;
        e.path = r.path;
        e.resolved = r.resolved;
        e.seconds = pending_seconds;
        e.text = pending_text;
        pending_seconds = -1;
        pending_text.clear();
        out.push_back(std::move(e));
        if (out.size() >= opt.max_entries) break;
    }
    return out;
}

std::string write(const std::vector<Entry> &entries, const WriteOptions &opt) {
    std::string out = "#EXTM3U\n";
    auto clean = [](std::string s) {
        for (char &c : s)
            if (c == '\n' || c == '\r') c = ' ';
        return s;
    };
    for (const Entry &e : entries) {
        if (!e.text.empty() || e.seconds >= 0) out += "#EXTINF:" + std::to_string(e.seconds >= 0 ? e.seconds : -1) + "," + clean(e.text) + "\n";
        std::string p = e.path;
        if (e.resolved && !p.empty() && p[0] == '/' && !opt.playlist_dir.empty() && (!opt.same_drive || opt.same_drive(opt.playlist_dir, p))) {
            const std::string rel = relative_to(normalize(opt.playlist_dir), p);
            if (!rel.empty()) p = rel;
        }
        out += clean(p) + "\n";
    }
    return out;
}

int64_t total_seconds(const std::vector<Entry> &entries) {
    int64_t t = 0;
    bool any = false;
    for (const Entry &e : entries)
        if (e.seconds >= 0) {
            t += e.seconds;
            any = true;
        }
    return any ? t : -1;
}

}  // namespace m3u
}  // namespace vinyl
