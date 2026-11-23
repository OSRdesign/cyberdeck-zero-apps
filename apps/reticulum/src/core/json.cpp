/*
 * SPDX-License-Identifier: MIT
 *
 * The JSON value, parser and writer (see json.hpp).
 */
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ret {
namespace json {

namespace {

const Value &null_value()
{
    static const Value v;
    return v;
}

const std::string &empty_string()
{
    static const std::string s;
    return s;
}

constexpr int kMaxDepth = 32;

struct Parser {
    const std::string &t;
    std::size_t i = 0;

    explicit Parser(const std::string &text) : t(text) {}

    void ws()
    {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }

    bool literal(const char *word)
    {
        std::size_t n = 0;
        while (word[n]) ++n;
        if (t.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }

    static void utf8(std::string &out, unsigned cp)
    {
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

    bool hex4(unsigned &out)
    {
        if (i + 4 > t.size()) return false;
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        i += 4;
        out = v;
        return true;
    }

    bool str(std::string &out)
    {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            const unsigned char c = static_cast<unsigned char>(t[i++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (i >= t.size()) return false;
            const char e = t[i++];
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
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return false;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool num(double &out)
    {
        const std::size_t start = i;
        if (i < t.size() && t[i] == '-') ++i;
        if (i >= t.size()) return false;
        if (t[i] == '0') ++i;
        else if (t[i] >= '1' && t[i] <= '9') {
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
        } else {
            return false;
        }
        if (i < t.size() && t[i] == '.') {
            ++i;
            const std::size_t d = i;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
            if (i == d) return false;
        }
        if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
            ++i;
            if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
            const std::size_t d = i;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
            if (i == d) return false;
        }
        out = std::strtod(t.substr(start, i - start).c_str(), nullptr);
        return std::isfinite(out);
    }

    bool value(Value &out, int depth)
    {
        if (depth > kMaxDepth) return false;
        ws();
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '{') {
            ++i;
            out = Value::object();
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return false;
                ++i;
                Value v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (i >= t.size()) return false;
                if (t[i] == ',') {
                    ++i;
                    continue;
                }
                if (t[i] == '}') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out = Value::array();
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                Value v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (i >= t.size()) return false;
                if (t[i] == ',') {
                    ++i;
                    continue;
                }
                if (t[i] == ']') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string s;
            if (!str(s)) return false;
            out = Value::string(std::move(s));
            return true;
        }
        if (c == 't') {
            if (!literal("true")) return false;
            out = Value::boolean(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            out = Value::boolean(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            out = Value();
            return true;
        }
        double n = 0;
        if (!num(n)) return false;
        out = Value::number(n);
        return true;
    }
};

void escape(std::string &out, const std::string &s)
{
    out += '"';
    for (const unsigned char c : s) {
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
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void write(std::string &out, const Value &v)
{
    switch (v.kind()) {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Value::Kind::Number: out += number_text(v.as_number()); break;
    case Value::Kind::String: escape(out, v.as_string()); break;
    case Value::Kind::Array: {
        out += '[';
        bool first = true;
        for (const Value &item : v.items()) {
            if (!first) out += ',';
            first = false;
            write(out, item);
        }
        out += ']';
        break;
    }
    case Value::Kind::Object: {
        out += '{';
        bool first = true;
        for (const auto &kv : v.members()) {
            if (!first) out += ',';
            first = false;
            escape(out, kv.first);
            out += ':';
            write(out, kv.second);
        }
        out += '}';
        break;
    }
    }
}

} // namespace

Value Value::boolean(bool b)
{
    Value v;
    v.kind_ = Kind::Bool;
    v.bool_ = b;
    return v;
}

Value Value::number(double n)
{
    Value v;
    v.kind_ = Kind::Number;
    v.num_ = n;
    return v;
}

Value Value::string(std::string s)
{
    Value v;
    v.kind_ = Kind::String;
    v.str_ = std::move(s);
    return v;
}

Value Value::array()
{
    Value v;
    v.kind_ = Kind::Array;
    return v;
}

Value Value::object()
{
    Value v;
    v.kind_ = Kind::Object;
    return v;
}

const std::string &Value::as_string() const
{
    return kind_ == Kind::String ? str_ : empty_string();
}

const Value &Value::operator[](const std::string &key) const
{
    if (kind_ == Kind::Object)
        for (const auto &kv : obj_)
            if (kv.first == key) return kv.second;
    return null_value();
}

const Value &Value::operator[](std::size_t index) const
{
    if (kind_ == Kind::Array && index < arr_.size()) return arr_[index];
    return null_value();
}

bool Value::has(const std::string &key) const
{
    if (kind_ != Kind::Object) return false;
    for (const auto &kv : obj_)
        if (kv.first == key) return true;
    return false;
}

std::size_t Value::size() const
{
    if (kind_ == Kind::Array) return arr_.size();
    if (kind_ == Kind::Object) return obj_.size();
    return 0;
}

Value &Value::set(const std::string &key, Value v)
{
    if (kind_ != Kind::Object) *this = Value::object();
    for (auto &kv : obj_)
        if (kv.first == key) {
            kv.second = std::move(v);
            return *this;
        }
    obj_.emplace_back(key, std::move(v));
    return *this;
}

Value &Value::push(Value v)
{
    if (kind_ != Kind::Array) *this = Value::array();
    arr_.push_back(std::move(v));
    return *this;
}

bool parse(const std::string &text, Value &out)
{
    Parser p(text);
    Value v;
    if (!p.value(v, 0)) return false;
    p.ws();
    if (p.i != text.size()) return false;
    out = std::move(v);
    return true;
}

std::string dump(const Value &v)
{
    std::string out;
    write(out, v);
    return out;
}

std::string number_text(double n)
{
    char buf[40];
    if (std::fabs(n) < 1e15 && n == std::floor(n)) std::snprintf(buf, sizeof buf, "%.0f", n);
    else std::snprintf(buf, sizeof buf, "%.15g", n);
    return buf;
}

} // namespace json
} // namespace ret
