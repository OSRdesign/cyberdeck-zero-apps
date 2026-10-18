/*
 * SPDX-License-Identifier: MIT
 */
#include "json.hpp"

#include "util.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rssreader {

const Json *Json::get(const std::string &key) const
{
    if (type != Type::Object) return nullptr;
    for (const auto &m : obj)
        if (m.first == key) return &m.second;
    return nullptr;
}

std::string Json::get_string(const std::string &key, const std::string &def) const
{
    const Json *v = get(key);
    return (v && v->type == Type::String) ? v->str : def;
}

double Json::get_double(const std::string &key, double def) const
{
    const Json *v = get(key);
    return (v && v->type == Type::Number) ? v->num : def;
}

std::int64_t Json::get_int(const std::string &key, std::int64_t def) const
{
    const Json *v = get(key);
    if (!v || v->type != Type::Number || !std::isfinite(v->num)) return def;
    if (v->num > 9.0e18 || v->num < -9.0e18) return def;
    return static_cast<std::int64_t>(v->num);
}

bool Json::get_bool(const std::string &key, bool def) const
{
    const Json *v = get(key);
    return (v && v->type == Type::Bool) ? v->b : def;
}

Json &Json::set(const std::string &key, Json value)
{
    type = Type::Object;
    for (auto &m : obj)
        if (m.first == key) {
            m.second = std::move(value);
            return m.second;
        }
    obj.emplace_back(key, std::move(value));
    return obj.back().second;
}

Json &Json::push(Json value)
{
    type = Type::Array;
    arr.push_back(std::move(value));
    return arr.back();
}

namespace {

constexpr std::size_t kMaxJson = 32u * 1024 * 1024;
constexpr int kMaxDepth = 64;

struct Reader {
    const std::string &s;
    std::size_t i = 0;
    explicit Reader(const std::string &text) : s(text) {}

    void ws()
    {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
            } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                std::size_t e = s.find("*/", i + 2);
                i = (e == std::string::npos) ? s.size() : e + 2;
            } else {
                return;
            }
        }
    }

    static int hexval(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(std::uint32_t &v)
    {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            int d = hexval(s[i + static_cast<std::size_t>(k)]);
            if (d < 0) return false;
            v = v * 16 + static_cast<std::uint32_t>(d);
        }
        i += 4;
        return true;
    }

    bool string(std::string &out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    std::size_t save = i;
                    i += 2;
                    std::uint32_t lo;
                    if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else i = save;
                }
                append_utf8(out, cp);
                break;
            }
            default: out += e; break;   /* tolerant: an unknown escape is the character itself */
            }
        }
        return false;
    }

    bool value(Json &out, int depth)
    {
        if (depth > kMaxDepth) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') {
            ++i;
            out = Json::object();
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out = Json::array();
            for (;;) {
                ws();
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                Json v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') {
            out = Json::string("");
            return string(out.str);
        }
        if (s.compare(i, 4, "true") == 0) { i += 4; out = Json::boolean(true); return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = Json::boolean(false); return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out = Json::null(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t st = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
            std::string tok = s.substr(st, i - st);
            char *end = nullptr;
            double d = std::strtod(tok.c_str(), &end);
            if (end == tok.c_str() || *end != '\0' || !std::isfinite(d)) return false;
            out = Json::number(d);
            return true;
        }
        return false;
    }
};

void dump(const Json &v, std::string &out, bool pretty, int indent)
{
    auto nl = [&](int level) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 1, ' ');
    };
    switch (v.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += v.b ? "true" : "false"; break;
    case Json::Type::Number: {
        char buf[40];
        if (std::isfinite(v.num) && v.num == std::floor(v.num) && std::fabs(v.num) < 9.0e15)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.num));
        else if (std::isfinite(v.num))
            std::snprintf(buf, sizeof buf, "%.10g", v.num);
        else
            std::snprintf(buf, sizeof buf, "0");
        out += buf;
        break;
    }
    case Json::Type::String: out += json_quote(v.str); break;
    case Json::Type::Array:
        out += '[';
        for (std::size_t k = 0; k < v.arr.size(); ++k) {
            if (k) out += ',';
            nl(indent + 1);
            dump(v.arr[k], out, pretty, indent + 1);
        }
        if (!v.arr.empty()) nl(indent);
        out += ']';
        break;
    case Json::Type::Object:
        out += '{';
        for (std::size_t k = 0; k < v.obj.size(); ++k) {
            if (k) out += ',';
            nl(indent + 1);
            out += json_quote(v.obj[k].first);
            out += pretty ? ": " : ":";
            dump(v.obj[k].second, out, pretty, indent + 1);
        }
        if (!v.obj.empty()) nl(indent);
        out += '}';
        break;
    }
}

} // namespace

bool json_parse(const std::string &text, Json &out)
{
    out = Json();
    if (text.size() > kMaxJson) return false;
    Reader r(text);
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) r.i = 3;
    Json v;
    if (!r.value(v, 0)) return false;
    r.ws();
    if (r.i != text.size()) return false;   /* trailing garbage: the file is damaged */
    out = std::move(v);
    return true;
}

std::string json_dump(const Json &value, bool pretty)
{
    std::string out;
    dump(value, out, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

std::string json_quote(const std::string &s)
{
    std::string out = "\"";
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

} // namespace rssreader
