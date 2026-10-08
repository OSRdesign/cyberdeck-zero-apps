/*
 * SPDX-License-Identifier: MIT
 */

#include "preset_feed.hpp"

#include "clock_policy.hpp"
#include "store.hpp"
#include "util.hpp"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace meshzero {

namespace {

/* ---------------------------------------------------------------- a small strict JSON reader (the feed has nested objects) */

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::string numtext;                                   // the number as written
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal *get(const char *key) const
    {
        for (const auto &kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct Reader {
    explicit Reader(const std::string &text) : s(text) {}
    const std::string &s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    bool fail(const char *m)
    {
        if (err.empty()) err = m;
        return false;
    }
    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool lit(const char *w)
    {
        const size_t n = std::strlen(w);
        if (s.compare(i, n, w) != 0) return fail("bad literal");
        i += n;
        return true;
    }
    static void put_utf8(std::string &o, uint32_t cp)
    {
        if (cp < 0x80) o.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool string(std::string &out)
    {
        if (i >= s.size() || s[i] != '"') return fail("string expected");
        ++i;
        out.clear();
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in a string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (i >= s.size()) return fail("bad escape");
            const char e = s[i++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'u': {
                if (i + 4 > s.size()) return fail("bad \\u escape");
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s[i++];
                    int v;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    else return fail("bad \\u escape");
                    cp = cp << 4 | static_cast<uint32_t>(v);
                }
                if (cp >= 0xD800 && cp < 0xE000) return fail("surrogate in a string");
                put_utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(JVal &v)
    {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        const size_t digits = i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        if (i == digits) return fail("bad number");
        v.type = JVal::Num;
        v.numtext = s.substr(start, i - start);
        char *end = nullptr;
        v.num = std::strtod(v.numtext.c_str(), &end);
        if (end == v.numtext.c_str() || *end != '\0' || !std::isfinite(v.num)) return fail("bad number");
        return true;
    }
    bool value(JVal &v)
    {
        if (++depth > 12) return fail("nested too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        bool ok;
        const char c = s[i];
        if (c == '{') {
            ++i;
            v.type = JVal::Obj;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; ok = true; }
            else {
                ok = true;
                for (;;) {
                    std::string key;
                    ws();
                    if (!string(key)) { ok = false; break; }
                    ws();
                    if (i >= s.size() || s[i] != ':') { ok = fail("':' expected"); break; }
                    ++i;
                    JVal child;
                    if (!value(child)) { ok = false; break; }
                    v.obj.emplace_back(std::move(key), std::move(child));
                    if (v.obj.size() > 200) { ok = fail("too many keys"); break; }
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    ok = fail("',' or '}' expected");
                    break;
                }
            }
        } else if (c == '[') {
            ++i;
            v.type = JVal::Arr;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; ok = true; }
            else {
                ok = true;
                for (;;) {
                    JVal child;
                    if (!value(child)) { ok = false; break; }
                    v.arr.push_back(std::move(child));
                    if (v.arr.size() > 500) { ok = fail("too many items"); break; }
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    ok = fail("',' or ']' expected");
                    break;
                }
            }
        } else if (c == '"') {
            v.type = JVal::Str;
            ok = string(v.str);
        } else if (c == 't') {
            v.type = JVal::Bool;
            v.b = true;
            ok = lit("true");
        } else if (c == 'f') {
            v.type = JVal::Bool;
            ok = lit("false");
        } else if (c == 'n') {
            v.type = JVal::Null;
            ok = lit("null");
        } else {
            ok = number(v);
        }
        --depth;
        return ok;
    }
};

/* A number written as a string ("915.800") or as a JSON number. Plain digits with one optional decimal point only. */
bool plain_number(const JVal *v, double &out)
{
    if (!v) return false;
    std::string t;
    if (v->type == JVal::Str) t = v->str;
    else if (v->type == JVal::Num) t = v->numtext;
    else return false;
    if (t.empty() || t.size() > 12) return false;
    int dots = 0;
    for (char c : t) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    if (dots > 1 || t.front() == '.' || t.back() == '.') return false;
    out = std::strtod(t.c_str(), nullptr);
    return true;
}

bool whole(double v, int lo, int hi, int &out)
{
    if (v != std::floor(v) || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool good_name(const std::string &n)
{
    if (n.empty() || n.size() > 48) return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7F) return false;
    return sanitize_utf8(n) == n && n.front() != ' ' && n.back() != ' ';
}

bool check_one(const RadioPreset &p, std::string &why)
{
    if (!good_name(p.name)) { why = "bad name"; return false; }
    RadioSettings s;
    s.name = "x";
    s.freq_mhz = p.freq_mhz;
    s.bw_khz = p.bw_khz;
    s.sf = p.sf;
    s.cr = p.cr;
    const std::string bad = validate_radio(s);                 // frequency 137..2500, a LoRa bandwidth (62.5, 125, 250, ...), SF 5..12, CR 5..8
    if (!bad.empty()) { why = p.name + ": " + bad; return false; }
    return true;
}

} // namespace

bool validate_preset_list(const std::vector<RadioPreset> &list, std::string &why)
{
    if (list.size() < kPresetMinEntries) { why = "too few entries"; return false; }
    if (list.size() > kPresetMaxEntries) { why = "too many entries"; return false; }
    for (size_t i = 0; i < list.size(); ++i) {
        if (!check_one(list[i], why)) return false;
        for (size_t k = 0; k < i; ++k)
            if (list[k].name == list[i].name) { why = "duplicate name " + list[i].name; return false; }
    }
    return true;
}

bool parse_preset_feed(const std::string &json, std::vector<RadioPreset> &out, std::string &why)
{
    out.clear();
    why.clear();
    if (json.empty()) { why = "empty answer"; return false; }
    if (json.size() > kPresetFeedMaxBytes) { why = "answer too large"; return false; }
    Reader r(json);
    JVal root;
    if (!r.value(root)) { why = "bad JSON: " + r.err; return false; }
    r.ws();
    if (r.i != json.size()) { why = "bad JSON: text after the value"; return false; }
    if (root.type != JVal::Obj) { why = "not an object"; return false; }
    const JVal *cfg = root.get("config");
    const JVal *sug = cfg && cfg->type == JVal::Obj ? cfg->get("suggested_radio_settings") : nullptr;
    const JVal *entries = sug && sug->type == JVal::Obj ? sug->get("entries") : nullptr;
    if (!entries || entries->type != JVal::Arr) { why = "no suggested_radio_settings.entries list"; return false; }
    std::vector<RadioPreset> list;
    for (const JVal &e : entries->arr) {
        if (e.type != JVal::Obj) { why = "an entry is not an object"; return false; }
        const JVal *title = e.get("title");
        if (!title || title->type != JVal::Str) { why = "an entry has no title"; return false; }
        RadioPreset p;
        p.name = title->str;
        double f = 0, bw = 0, sf = 0, cr = 0;
        if (!plain_number(e.get("frequency"), f) || !plain_number(e.get("bandwidth"), bw) ||
            !plain_number(e.get("spreading_factor"), sf) || !plain_number(e.get("coding_rate"), cr)) {
            why = p.name + ": a number is missing or not plain";
            return false;
        }
        p.freq_mhz = f;
        p.bw_khz = bw;
        if (!whole(sf, 5, 12, p.sf) || !whole(cr, 5, 8, p.cr)) {
            why = p.name + ": spreading factor or coding rate out of range";
            return false;
        }
        list.push_back(std::move(p));
    }
    if (!validate_preset_list(list, why)) return false;
    out = std::move(list);
    return true;
}

/* ---------------------------------------------------------------- the cache file */

namespace {
std::string fmt_num(double v)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.4f", v);
    return b;
}
} // namespace

bool save_preset_cache(const std::string &dir, const std::vector<RadioPreset> &list, const std::string &date, const std::string &source)
{
    std::string why;
    if (!validate_preset_list(list, why)) return false;
    std::string all = "{\"date\":" + json_quote(date) + ",\"src\":" + json_quote(source) + ",\"n\":" + std::to_string(list.size()) + "}\n";
    for (const RadioPreset &p : list)
        all += "{\"nm\":" + json_quote(p.name) + ",\"f\":" + fmt_num(p.freq_mhz) + ",\"bw\":" + fmt_num(p.bw_khz) + ",\"sf\":" + std::to_string(p.sf) +
               ",\"cr\":" + std::to_string(p.cr) + "}\n";
    const std::string path = dir + "/presets.jsonl", tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(all.data(), 1, all.size(), f) == all.size();
    ok = std::fflush(f) == 0 && ok;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool load_preset_cache(const std::string &dir, std::vector<RadioPreset> &list, std::string &date, std::string &source)
{
    list.clear();
    std::ifstream in(dir + "/presets.jsonl", std::ios::binary);
    if (!in) return false;
    std::string line;
    JsonObject head;
    if (!std::getline(in, line) || !parse_json_object(line, head) || !head.count("date") || !head.count("n")) return false;
    const std::string d = head["date"].is_string ? head["date"].str : std::string();
    const size_t n = static_cast<size_t>(head["n"].num);
    std::vector<RadioPreset> out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        JsonObject o;
        if (!parse_json_object(line, o) || !o.count("nm") || !o["nm"].is_string || !o.count("f") || !o.count("bw") || !o.count("sf") || !o.count("cr"))
            return false;                                           // a damaged line: the whole copy is dropped
        RadioPreset p;
        p.name = o["nm"].str;
        p.freq_mhz = o["f"].num;
        p.bw_khz = o["bw"].num;
        p.sf = static_cast<int>(o["sf"].num);
        p.cr = static_cast<int>(o["cr"].num);
        out.push_back(std::move(p));
    }
    std::string why;
    if (out.size() != n || !validate_preset_list(out, why)) return false;
    if (d != "unknown" && (d.size() != 10 || d[4] != '-' || d[7] != '-')) return false;     // "unknown": saved while the clock was not set
    list = std::move(out);
    date = d;
    source = head.count("src") && head["src"].is_string ? head["src"].str : std::string();
    return true;
}

std::string fetch_date_text(uint32_t unix_time)
{
    if (unix_time < kMinPlausibleTime) return "";
    return format_local_datetime(unix_time).substr(0, 10);
}

/* ---------------------------------------------------------------- online? */

bool route_table_has_default(const std::string &text)
{
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);                                          // the header: Iface Destination Gateway Flags ...
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string iface, dest, gw, flags;
        if (!(ls >> iface >> dest >> gw >> flags)) continue;
        if (iface == "lo") continue;
        if (dest == "00000000") {
            const unsigned long fl = std::strtoul(flags.c_str(), nullptr, 16);
            if (fl & 0x1) return true;                               // RTF_UP
        }
    }
    return false;
}

bool network_online()
{
    if (const char *e = std::getenv("MESHHOP_ONLINE"); e && *e) return *e != '0';
    std::ifstream in("/proc/net/route");
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return route_table_has_default(ss.str());
}

/* ---------------------------------------------------------------- the background download */

PresetFetcher::PresetFetcher(std::string data_dir) : dir_(std::move(data_dir))
{
    const char *c = std::getenv("MESHHOP_CURL");
    curl_ = c && *c ? c : "/usr/bin/curl";
    const char *u = std::getenv("MESHHOP_PRESETS_URL");
    url_ = u && *u ? u : presets_source_url();
    tmp_ = dir_ + "/presets.download";
}

PresetFetcher::~PresetFetcher()
{
    abort();
}

void PresetFetcher::finish(State s, const std::string &msg)
{
    state_ = s;
    message_ = msg;
    pid_ = 0;
}

void PresetFetcher::abort()
{
    if (state_ == State::Running && pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int st = 0;
        ::waitpid(pid_, &st, 0);
        std::remove(tmp_.c_str());
        finish(State::Failed, "stopped");
    }
}

bool PresetFetcher::start(uint64_t now_ms)
{
    if (state_ == State::Running) return true;
    list_.clear();
    if (::access(curl_.c_str(), X_OK) != 0) {
        finish(State::Failed, "curl not found (" + curl_ + ")");
        return false;
    }
    std::remove(tmp_.c_str());
    std::vector<std::string> args = {curl_, "-sS", "-f", "-L", "--max-redirs", "3", "--connect-timeout", "6", "--max-time", "15",
                                     "--max-filesize", std::to_string(kPresetFeedMaxBytes), "-A", "mesh-hop", "-H", "Accept: application/json"};
    if (url_.rfind("https://", 0) == 0) {
        args.push_back("--proto");
        args.push_back("=https");
        args.push_back("--proto-redir");
        args.push_back("=https");
    }
    args.push_back("-o");
    args.push_back(tmp_);
    args.push_back(url_);
    std::vector<char *> argv;
    for (std::string &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, curl_.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        finish(State::Failed, std::string("cannot start curl: ") + std::strerror(rc));
        return false;
    }
    pid_ = pid;
    started_ms_ = now_ms;
    state_ = State::Running;
    message_ = "downloading";
    return true;
}

PresetFetcher::State PresetFetcher::poll(uint64_t now_ms)
{
    if (state_ != State::Running) return state_;
    int st = 0;
    const pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == 0) {
        if (now_ms - started_ms_ > kDeadlineMs) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, &st, 0);
            std::remove(tmp_.c_str());
            finish(State::Failed, "no answer in time");
        }
        return state_;
    }
    if (r < 0) {
        std::remove(tmp_.c_str());
        finish(State::Failed, std::string("wait failed: ") + std::strerror(errno));
        return state_;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        std::remove(tmp_.c_str());
        finish(State::Failed, WIFEXITED(st) ? "curl failed, exit code " + std::to_string(WEXITSTATUS(st)) : std::string("curl was killed"));
        return state_;
    }
    std::string body;
    {
        std::ifstream in(tmp_, std::ios::binary);
        if (in) {
            char buf[4096];
            while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
                body.append(buf, static_cast<size_t>(in.gcount()));
                if (body.size() > kPresetFeedMaxBytes) break;
            }
        }
    }
    std::remove(tmp_.c_str());
    std::string why;
    std::vector<RadioPreset> list;
    if (!parse_preset_feed(body, list, why)) {
        finish(State::Failed, "list refused: " + why);
        return state_;
    }
    list_ = std::move(list);
    finish(State::Done, std::to_string(list_.size()) + " entries");
    return state_;
}

} // namespace meshzero
