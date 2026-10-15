/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: a small JSON reader (see minijson.hpp).
 */
#include "minijson.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace nt {
namespace json {
namespace {

constexpr int kMaxDepth = 16;

struct Reader {
    const std::string &s;
    std::size_t i;

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    bool eat(char c)
    {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    static void put_utf8(std::string &out, std::uint32_t cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
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
    bool hex4(std::uint32_t &v)
    {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string &out)
    {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c == '\n' || c == '\r') continue; /* a line cut in pieces: see the header */
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
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
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    const std::size_t save = i;
                    i += 2;
                    std::uint32_t lo = 0;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else i = save;
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = '?'; /* a lone surrogate */
                put_utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool literal(const char *word)
    {
        std::size_t k = 0;
        while (word[k]) {
            if (i + k >= s.size() || s[i + k] != word[k]) return false;
            ++k;
        }
        i += k;
        return true;
    }
    bool number(Value &v)
    {
        const std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                s[i] == '-' || s[i] == '+'))
            ++i;
        if (i == start || i - start > 40) return false;
        v.kind = Value::Kind::Number;
        v.text = s.substr(start, i - start);
        char *end = nullptr;
        errno = 0;
        v.number = std::strtod(v.text.c_str(), &end);
        return end && *end == '\0' && errno != ERANGE && std::isfinite(v.number);
    }
    bool value(Value &v, int depth)
    {
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') {
            v.kind = Value::Kind::String;
            return string(v.text);
        }
        if (c == '{' || c == '[') {
            v.kind = Value::Kind::Nested;
            return skip_nested(depth + 1);
        }
        if (literal("true")) {
            v.kind = Value::Kind::Bool;
            v.text = "true";
            return true;
        }
        if (literal("false")) {
            v.kind = Value::Kind::Bool;
            v.text = "false";
            return true;
        }
        if (literal("null")) {
            v.kind = Value::Kind::Null;
            return true;
        }
        return number(v);
    }
    bool skip_nested(int depth)
    {
        if (depth > kMaxDepth) return false;
        const char open = s[i++];
        const char close = open == '{' ? '}' : ']';
        if (eat(close)) return true;
        for (;;) {
            if (open == '{') {
                std::string key;
                if (!string(key) || !eat(':')) return false;
            }
            Value v;
            if (!value(v, depth)) return false;
            if (eat(',')) continue;
            return eat(close);
        }
    }
};

} // namespace

bool parse_object(const std::string &text, std::size_t pos, Object &out, std::size_t *end)
{
    out.clear();
    Reader r{text, pos};
    if (!r.eat('{')) return false;
    if (r.eat('}')) {
        if (end) *end = r.i;
        return true;
    }
    for (;;) {
        std::string key;
        Value v;
        if (!r.string(key) || !r.eat(':') || !r.value(v, 1)) {
            out.clear();
            return false;
        }
        out[key] = std::move(v);
        if (r.eat(',')) continue;
        if (!r.eat('}')) {
            out.clear();
            return false;
        }
        if (end) *end = r.i;
        return true;
    }
}

std::string get_string(const Object &o, const std::string &key, const std::string &def)
{
    const auto it = o.find(key);
    return it != o.end() && it->second.kind == Value::Kind::String ? it->second.text : def;
}

double get_number(const Object &o, const std::string &key, double def)
{
    const auto it = o.find(key);
    return it != o.end() && it->second.kind == Value::Kind::Number ? it->second.number : def;
}

bool has(const Object &o, const std::string &key)
{
    return o.find(key) != o.end();
}

} // namespace json
} // namespace nt
