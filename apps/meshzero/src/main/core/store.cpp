/*
 * SPDX-License-Identifier: MIT
 */

#include "store.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace meshzero {

/* ---------------------------------------------------------------- JSON lite */

std::string json_quote(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

namespace {

void append_utf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct Cursor {
    const std::string &s;
    size_t i = 0;
    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool eat(char c)
    {
        ws();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }
};

bool parse_hex4(const std::string &s, size_t i, uint32_t &out)
{
    if (i + 4 > s.size()) return false;
    out = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char c = s[i + k];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        out = out << 4 | static_cast<uint32_t>(v);
    }
    return true;
}

bool parse_string(Cursor &c, std::string &out)
{
    if (!c.eat('"')) return false;
    out.clear();
    while (c.i < c.s.size()) {
        const char ch = c.s[c.i++];
        if (ch == '"') return true;
        if (ch != '\\') { out.push_back(ch); continue; }
        if (c.i >= c.s.size()) return false;
        const char e = c.s[c.i++];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '"': case '\\': case '/': out.push_back(e); break;
        case 'u': {
            uint32_t cp;
            if (!parse_hex4(c.s, c.i, cp)) return false;
            c.i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && c.i + 6 <= c.s.size() && c.s[c.i] == '\\' && c.s[c.i + 1] == 'u') {
                uint32_t lo;
                if (parse_hex4(c.s, c.i + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    c.i += 6;
                }
            }
            append_utf8(out, cp);
            break;
        }
        default: return false;
        }
    }
    return false;
}

} // namespace

bool parse_json_object(const std::string &line, JsonObject &out)
{
    out.clear();
    Cursor c{line};
    if (!c.eat('{')) return false;
    if (c.eat('}')) return true;
    for (;;) {
        std::string key;
        if (!parse_string(c, key) || !c.eat(':')) return false;
        c.ws();
        JsonValue v;
        if (c.i < line.size() && line[c.i] == '"') {
            v.is_string = true;
            if (!parse_string(c, v.str)) return false;
        } else {
            const size_t start = c.i;
            while (c.i < line.size() && line[c.i] != ',' && line[c.i] != '}' && line[c.i] != ' ') ++c.i;
            const std::string tok = line.substr(start, c.i - start);
            if (tok == "true") v.num = 1;
            else if (tok == "false" || tok == "null") v.num = 0;
            else {
                char *end = nullptr;
                v.num = std::strtod(tok.c_str(), &end);
                if (end == tok.c_str()) return false;
            }
        }
        out[key] = v;
        if (c.eat(',')) continue;
        return c.eat('}');
    }
}

/* ---------------------------------------------------------------- store */

namespace {

std::string str_of(const JsonObject &o, const char *k)
{
    const auto it = o.find(k);
    return it == o.end() ? std::string() : (it->second.is_string ? it->second.str : std::to_string(it->second.num));
}

double num_of(const JsonObject &o, const char *k, double def = 0)
{
    const auto it = o.find(k);
    return it == o.end() || it->second.is_string ? def : it->second.num;
}

std::string fmt_double(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string message_line(const Message &m)
{
    std::string s = "{\"i\":" + std::to_string(m.seq) + ",\"k\":" + json_quote(m.conv) + ",\"d\":" + (m.dir == Dir::Out ? "1" : "0") +
                    ",\"t\":" + std::to_string(m.ts) + ",\"s\":" + std::to_string(m.sender_ts) + ",\"n\":" + json_quote(m.sender) +
                    ",\"x\":" + json_quote(m.text) + ",\"st\":" + std::to_string(static_cast<int>(m.state)) +
                    ",\"h\":" + std::to_string(m.hops);
    if (m.has_snr) s += ",\"snr\":" + fmt_double(m.snr);
    if (!m.note.empty()) s += ",\"e\":" + json_quote(m.note);
    return s + "}\n";
}

std::string contact_line(const ContactRec &r)
{
    const Contact &c = r.c;
    std::string s = "{\"pk\":\"" + to_hex(c.key) + "\",\"ty\":" + std::to_string(c.type) + ",\"fl\":" + std::to_string(c.flags) +
                    ",\"pl\":" + std::to_string(c.out_path_len) + ",\"pa\":\"" + to_hex(c.out_path.data(), c.out_path.size()) +
                    "\",\"nm\":" + json_quote(c.name) + ",\"la\":" + std::to_string(c.last_advert) +
                    ",\"lat\":" + fmt_double(c.lat) + ",\"lon\":" + fmt_double(c.lon) + ",\"lm\":" + std::to_string(c.lastmod) +
                    ",\"hl\":" + std::to_string(r.heard_local);
    if (r.has_snr) s += ",\"snr\":" + fmt_double(r.snr);
    return s + "}\n";
}

template <typename Fn> void for_each_line(const std::string &file, Fn fn)
{
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) fn(line);
    }
}

} // namespace

Store::Store(std::string dir) : dir_(std::move(dir)) {}

bool Store::migrate_legacy_dir(const std::string &old_dir, const std::string &new_dir)
{
    struct stat st;
    if (::stat(old_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (::lstat(new_dir.c_str(), &st) == 0) return false;      // the new one exists (or is something else): leave both
    return ::rename(old_dir.c_str(), new_dir.c_str()) == 0;
}

std::string Store::default_dir()
{
    if (const char *d = std::getenv("MESHZERO_DATA"); d && *d) return d;
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x) {
        std::string nd = std::string(x) + "/meshzero";
        migrate_legacy_dir(std::string(x) + "/meshcore", nd);
        return nd;
    }
    if (const char *h = std::getenv("HOME"); h && *h) {
        std::string nd = std::string(h) + "/.local/share/meshzero";
        migrate_legacy_dir(std::string(h) + "/.local/share/meshcore", nd);
        return nd;
    }
    return "/tmp/meshzero";
}

static bool make_dirs(const std::string &dir)
{
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (i < dir.size()) cur.push_back(dir[i]);
    }
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Store::attach(Model &model)
{
    model_ = &model;
    healthy_ = make_dirs(dir_);
    if (healthy_) {
        load_contacts();
        load_channels();
        load_messages();
        load_read();
    }
    model.set_listener(this);
    return healthy_;
}

void Store::load_messages()
{
    std::map<uint32_t, Message> by_seq;
    std::vector<uint32_t> order;
    size_t lines = 0;
    for_each_line(path("messages.jsonl"), [&](const std::string &line) {
        ++lines;
        JsonObject o;
        if (!parse_json_object(line, o)) return;
        if (o.count("u")) {
            const auto it = by_seq.find(static_cast<uint32_t>(num_of(o, "u")));
            if (it == by_seq.end()) return;
            it->second.state = static_cast<MsgState>(static_cast<int>(num_of(o, "st")));
            it->second.note = str_of(o, "e");
            return;
        }
        if (!o.count("i") || !o.count("k")) return;
        Message m;
        m.seq = static_cast<uint32_t>(num_of(o, "i"));
        m.conv = str_of(o, "k");
        m.dir = num_of(o, "d") != 0 ? Dir::Out : Dir::In;
        m.ts = static_cast<uint32_t>(num_of(o, "t"));
        m.sender_ts = static_cast<uint32_t>(num_of(o, "s"));
        m.sender = str_of(o, "n");
        m.text = str_of(o, "x");
        const int st = static_cast<int>(num_of(o, "st"));
        m.state = st >= 0 && st <= static_cast<int>(MsgState::NoAck) ? static_cast<MsgState>(st) : MsgState::Received;
        m.hops = static_cast<int>(num_of(o, "h", -1));
        m.has_snr = o.count("snr") != 0;
        m.snr = num_of(o, "snr");
        m.note = str_of(o, "e");
        if (m.state == MsgState::Pending) {       // the app stopped while it was sending
            m.state = MsgState::Failed;
            m.note = "interrupted";
        }
        if (!by_seq.count(m.seq)) order.push_back(m.seq);
        by_seq[m.seq] = m;
    });
    for (uint32_t seq : order) model_->load_message(by_seq[seq]);
    // the lines of a long history: rewrite it bounded now rather than at the first message
    if (lines > model_->message_count() + 400) compact();
}

void Store::load_contacts()
{
    for_each_line(path("contacts.jsonl"), [&](const std::string &line) {
        JsonObject o;
        if (!parse_json_object(line, o)) return;
        ContactRec r;
        if (!from_hex(str_of(o, "pk"), r.c.key.data(), 32)) return;
        r.c.type = static_cast<uint8_t>(num_of(o, "ty"));
        r.c.flags = static_cast<uint8_t>(num_of(o, "fl"));
        r.c.out_path_len = static_cast<uint8_t>(num_of(o, "pl", 255));
        from_hex(str_of(o, "pa"), r.c.out_path.data(), 64);
        r.c.name = str_of(o, "nm");
        r.c.last_advert = static_cast<uint32_t>(num_of(o, "la"));
        r.c.lat = num_of(o, "lat");
        r.c.lon = num_of(o, "lon");
        r.c.lastmod = static_cast<uint32_t>(num_of(o, "lm"));
        r.heard_local = static_cast<uint32_t>(num_of(o, "hl"));
        r.has_snr = o.count("snr") != 0;
        r.snr = num_of(o, "snr");
        model_->load_contact(r);
    });
}

void Store::load_channels()
{
    for_each_line(path("channels.jsonl"), [&](const std::string &line) {      // the names first: load_channel flags the slots
        JsonObject o;
        if (parse_json_object(line, o) && o.count("ad")) model_->load_added(str_of(o, "ad"));
    });
    for_each_line(path("channels.jsonl"), [&](const std::string &line) {
        JsonObject o;
        if (!parse_json_object(line, o) || !o.count("idx")) return;
        ChannelRec c;
        c.idx = static_cast<uint8_t>(num_of(o, "idx"));
        c.name = str_of(o, "nm");
        c.empty = num_of(o, "em") != 0;
        model_->load_channel(c);
    });
}

void Store::load_read()
{
    for_each_line(path("read.txt"), [&](const std::string &line) {
        const size_t sp = line.rfind(' ');
        if (sp == std::string::npos) return;
        model_->load_read(line.substr(0, sp), static_cast<uint32_t>(std::strtoul(line.c_str() + sp + 1, nullptr, 10)));
    });
}

bool Store::append_line(const char *name, const std::string &line)
{
    if (!healthy_) return false;
    FILE *f = std::fopen(path(name).c_str(), "ab");
    if (!f) { healthy_ = false; return false; }
    const bool ok = std::fwrite(line.data(), 1, line.size(), f) == line.size();
    std::fclose(f);
    if (!ok) healthy_ = false;
    return ok;
}

bool Store::rewrite(const char *name, const std::string &content)
{
    if (!healthy_) return false;
    const std::string tmp = path(name) + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) { healthy_ = false; return false; }
    bool ok = std::fwrite(content.data(), 1, content.size(), f) == content.size();
    ok = std::fflush(f) == 0 && ok;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path(name).c_str()) != 0) {
        std::remove(tmp.c_str());
        healthy_ = false;
        return false;
    }
    return true;
}

void Store::compact()
{
    if (!model_) return;
    std::string all;
    for (const Message &m : model_->all_messages()) all += message_line(m);
    rewrite("messages.jsonl", all);
    appended_ = 0;
}

void Store::message_added(const Message &m)
{
    append_line("messages.jsonl", message_line(m));
    if (++appended_ >= 300) compact();
}

void Store::message_state(uint32_t seq, MsgState state, const std::string &note)
{
    std::string line = "{\"u\":" + std::to_string(seq) + ",\"st\":" + std::to_string(static_cast<int>(state));
    if (!note.empty()) line += ",\"e\":" + json_quote(note);
    append_line("messages.jsonl", line + "}\n");
}

void Store::contacts_changed()
{
    if (!model_) return;
    std::string all;
    for (const ContactRec *r : model_->contacts_sorted()) all += contact_line(*r);
    rewrite("contacts.jsonl", all);
}

void Store::channels_changed()
{
    if (!model_) return;
    std::string all;
    for (const ChannelRec &c : model_->channels())
        all += "{\"idx\":" + std::to_string(c.idx) + ",\"nm\":" + json_quote(c.name) + ",\"em\":" + (c.empty ? "1" : "0") + "}\n";
    for (const std::string &n : model_->added_names()) all += "{\"ad\":" + json_quote(n) + "}\n";
    rewrite("channels.jsonl", all);
}

void Store::read_changed()
{
    if (!model_) return;
    std::string all;
    for (const auto &kv : model_->read_marks()) all += kv.first + " " + std::to_string(kv.second) + "\n";
    rewrite("read.txt", all);
}

} // namespace meshzero
