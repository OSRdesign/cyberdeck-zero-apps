/*
 * SPDX-License-Identifier: MIT
 */
#include "journal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <memory>

namespace cassette {
namespace {

/* A tiny JSON reader: objects, arrays, strings, integers, booleans, null. Enough for our own journal. */
struct J {
    enum T { Null, Bool, Int, Str, Arr, Obj } t = Null;
    bool b = false;
    long long i = 0;
    std::string s;
    std::vector<J> a;
    std::map<std::string, J> o;
    const J *get(const std::string &k) const {
        auto it = o.find(k);
        return it == o.end() ? nullptr : &it->second;
    }
};

struct Parser {
    const std::string &s;
    size_t p = 0;
    bool bad = false;
    explicit Parser(const std::string &str) : s(str) {}
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) p++;
    }
    bool eat(char c) {
        ws();
        if (p < s.size() && s[p] == c) {
            p++;
            return true;
        }
        return false;
    }
    std::string str() {
        std::string r;
        if (!eat('"')) {
            bad = true;
            return r;
        }
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c == '\\' && p < s.size()) {
                char e = s[p++];
                switch (e) {
                case 'n': r += '\n'; break;
                case 'r': r += '\r'; break;
                case 't': r += '\t'; break;
                case 'u': {
                    if (p + 4 > s.size()) {
                        bad = true;
                        return r;
                    }
                    unsigned v = static_cast<unsigned>(std::strtoul(s.substr(p, 4).c_str(), nullptr, 16));
                    p += 4;
                    r += v < 0x80 ? static_cast<char>(v) : '?';
                    break;
                }
                default: r += e;
                }
            } else {
                r += c;
            }
        }
        if (p >= s.size()) bad = true;
        else p++;
        return r;
    }
    J value(int depth) {
        J v;
        ws();
        if (p >= s.size() || depth > 8) {
            bad = true;
            return v;
        }
        char c = s[p];
        if (c == '{') {
            p++;
            v.t = J::Obj;
            if (eat('}')) return v;
            do {
                std::string k = str();
                if (!eat(':')) bad = true;
                if (bad) return v;
                v.o[k] = value(depth + 1);
            } while (!bad && eat(','));
            if (!eat('}')) bad = true;
        } else if (c == '[') {
            p++;
            v.t = J::Arr;
            if (eat(']')) return v;
            do v.a.push_back(value(depth + 1));
            while (!bad && eat(','));
            if (!eat(']')) bad = true;
        } else if (c == '"') {
            v.t = J::Str;
            v.s = str();
        } else if (s.compare(p, 4, "true") == 0) {
            v.t = J::Bool, v.b = true, p += 4;
        } else if (s.compare(p, 5, "false") == 0) {
            v.t = J::Bool, v.b = false, p += 5;
        } else if (s.compare(p, 4, "null") == 0) {
            p += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            char *end = nullptr;
            v.t = J::Int;
            v.i = std::strtoll(s.c_str() + p, &end, 10);
            p = static_cast<size_t>(end - s.c_str());
        } else {
            bad = true;
        }
        return v;
    }
};

std::string esc(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

long long num(const J *j, long long def = 0) { return j && j->t == J::Int ? j->i : def; }
std::string text(const J *j) { return j && j->t == J::Str ? j->s : std::string(); }

}  // namespace

std::string journal_dir(const std::string &dir) { return dir + "/.journal"; }

std::string journal_sanitize(const std::string &take) {
    std::string r;
    for (unsigned char c : take) r += (c == '/' || c < 0x20) ? '_' : static_cast<char>(c);
    if (r.empty() || r == "." || r == "..") r = "take";
    return r;
}

std::string journal_file(const std::string &dir, const std::string &take) {
    return journal_dir(dir) + "/" + journal_sanitize(take) + ".json";
}

std::string journal_to_json(const JournalEntry &e) {
    std::string o = "{\n";
    o += "  \"take\": " + esc(e.take) + ",\n";
    o += "  \"path\": " + esc(e.path) + ",\n";
    o += "  \"parts\": [";
    for (size_t k = 0; k < e.parts.size(); k++) o += (k ? ", " : "") + esc(e.parts[k]);
    o += "],\n";
    o += "  \"format\": {\"rate\": " + std::to_string(e.fmt.rate) + ", \"channels\": " + std::to_string(e.fmt.channels) +
         ", \"bits\": " + std::to_string(e.fmt.bits) + "},\n";
    o += "  \"device\": {\"name\": " + esc(e.device_name) + ", \"by_id\": " + esc(e.device_id) + "},\n";
    o += "  \"started\": {\"mono_ms\": " + std::to_string(e.started_mono_ms) + ", \"wall\": " + std::to_string(e.started_wall) + "},\n";
    o += "  \"writer\": {\"sync_interval_ms\": " + std::to_string(e.sync_interval_ms) + ", \"auto_split\": " +
         (e.auto_split ? "true" : "false") + "},\n";
    o += "  \"state\": " + esc(e.state) + ",\n";
    o += "  \"frames\": " + std::to_string(e.frames) + ",\n";
    o += "  \"frames_synced\": " + std::to_string(e.frames_synced) + ",\n";
    o += "  \"last_sync_wall\": " + std::to_string(e.last_sync_wall) + ",\n";
    o += "  \"paused_frames\": " + std::to_string(e.paused_frames) + ",\n";
    o += "  \"dropped_frames\": " + std::to_string(e.dropped_frames) + ",\n";
    o += "  \"reason\": " + esc(e.reason) + "\n}\n";
    return o;
}

bool journal_from_json(const std::string &textIn, JournalEntry &e) {
    Parser ps(textIn);
    J root = ps.value(0);
    if (ps.bad || root.t != J::Obj) return false;
    const J *take = root.get("take");
    const J *state = root.get("state");
    if (!take || take->t != J::Str || !state || state->t != J::Str) return false;
    JournalEntry r;
    r.take = take->s;
    r.state = state->s;
    r.path = text(root.get("path"));
    if (const J *pa = root.get("parts"); pa && pa->t == J::Arr)
        for (const J &x : pa->a)
            if (x.t == J::Str) r.parts.push_back(x.s);
    if (r.parts.empty() && !r.path.empty()) r.parts.push_back(r.path);
    if (const J *f = root.get("format")) {
        r.fmt.rate = static_cast<uint32_t>(num(f->get("rate"), 48000));
        r.fmt.channels = static_cast<uint16_t>(num(f->get("channels"), 1));
        r.fmt.bits = static_cast<uint16_t>(num(f->get("bits"), 16));
    }
    if (const J *d = root.get("device")) {
        r.device_name = text(d->get("name"));
        r.device_id = text(d->get("by_id"));
    }
    if (const J *st = root.get("started")) {
        r.started_mono_ms = num(st->get("mono_ms"));
        r.started_wall = num(st->get("wall"));
    }
    if (const J *w = root.get("writer")) {
        r.sync_interval_ms = static_cast<uint32_t>(num(w->get("sync_interval_ms"), 10000));
        const J *as = w->get("auto_split");
        r.auto_split = !as || as->t != J::Bool || as->b;
    }
    r.frames = static_cast<uint64_t>(num(root.get("frames")));
    r.frames_synced = static_cast<uint64_t>(num(root.get("frames_synced")));
    r.last_sync_wall = num(root.get("last_sync_wall"));
    r.paused_frames = static_cast<uint64_t>(num(root.get("paused_frames")));
    r.dropped_frames = static_cast<uint64_t>(num(root.get("dropped_frames")));
    r.reason = text(root.get("reason"));
    e = r;
    return true;
}

int journal_write(FileOps &ops, const std::string &dir, const JournalEntry &e) {
    const std::string jd = journal_dir(dir);
    int r = ops.mkdir(jd);
    if (r < 0) return r;
    const std::string fin = journal_file(dir, e.take);
    const std::string tmp = fin + ".tmp";
    int fd = ops.open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    const std::string body = journal_to_json(e);
    r = write_all(ops, fd, body.data(), body.size());
    if (r == 0) r = ops.fsync(fd);
    int c = ops.close(fd);
    if (r == 0) r = c;
    if (r == 0) r = ops.rename(tmp, fin);
    if (r < 0) {
        ops.unlink(tmp);
        return r;
    }
    return ops.fsync_dir(jd);
}

int journal_read(FileOps &ops, const std::string &file, JournalEntry &e) {
    StatInfo st;
    int r = ops.stat(file, st);
    if (r < 0) return r;
    if (st.size > (1u << 20)) return -EINVAL;
    int fd = ops.open(file, O_RDONLY, 0);
    if (fd < 0) return fd;
    std::string buf(static_cast<size_t>(st.size), '\0');
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t k = ops.pread(fd, &buf[got], buf.size() - got, got);
        if (k <= 0) break;
        got += static_cast<size_t>(k);
    }
    ops.close(fd);
    buf.resize(got);
    return journal_from_json(buf, e) ? 0 : -EINVAL;
}

}  // namespace cassette
