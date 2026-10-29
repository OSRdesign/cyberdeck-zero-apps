/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the small JSON reader (see json.hpp).
 */
#include "json.hpp"

#include <cerrno>
#include <cstdlib>

namespace klondike {
namespace json {

namespace {

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxText = 4u * 1024 * 1024;

class Parser {
public:
    explicit Parser(const std::string &t) : t_(t) {}

    bool value(Value &out, int depth)
    {
        if (depth > kMaxDepth) return fail("nested too deep");
        ws();
        if (i_ >= t_.size()) return fail("unexpected end");
        const char c = t_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (c == 't') return word("true", out, Value::Kind::Bool, true);
        if (c == 'f') return word("false", out, Value::Kind::Bool, false);
        if (c == 'n') return word("null", out, Value::Kind::Null, false);
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool end()
    {
        ws();
        return i_ == t_.size() || fail("text after the value");
    }

    std::string why;

private:
    bool fail(const char *w)
    {
        if (why.empty()) why = std::string(w) + " at byte " + std::to_string(i_);
        return false;
    }

    void ws()
    {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }

    bool word(const char *w, Value &out, Value::Kind kind, bool b)
    {
        std::size_t n = 0;
        while (w[n]) ++n;
        if (t_.compare(i_, n, w) != 0) return fail("bad literal");
        i_ += n;
        out.kind = kind;
        out.boolean = b;
        return true;
    }

    bool number(Value &out)
    {
        const std::size_t start = i_;
        if (t_[i_] == '-') ++i_;
        auto digits = [this] {
            const std::size_t s = i_;
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
            return i_ > s;
        };
        if (!digits()) return fail("bad number");
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            if (!digits()) return fail("bad number");
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (!digits()) return fail("bad number");
        }
        out.kind = Value::Kind::Number;
        out.text = t_.substr(start, i_ - start);
        return true;
    }

    static void put_utf8(std::string &s, unsigned cp)
    {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned &out)
    {
        if (i_ + 4 > t_.size()) return fail("bad \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string &out)
    {
        ++i_; /* the opening quote */
        out.clear();
        while (i_ < t_.size()) {
            const char c = t_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= t_.size()) break;
            const char e = t_[i_++];
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
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned lo = 0;
                    if (i_ + 1 < t_.size() && t_[i_] == '\\' && t_[i_ + 1] == 'u') {
                        i_ += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo >= 0xE000) return fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return fail("lone surrogate");
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return fail("lone surrogate");
                }
                put_utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Value &out, int depth)
    {
        ++i_;
        out.kind = Value::Kind::Array;
        ws();
        if (i_ < t_.size() && t_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            Value v;
            if (!value(v, depth + 1)) return false;
            out.items.push_back(std::move(v));
            ws();
            if (i_ >= t_.size()) return fail("unterminated array");
            if (t_[i_] == ',') {
                ++i_;
                continue;
            }
            if (t_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool object(Value &out, int depth)
    {
        ++i_;
        out.kind = Value::Kind::Object;
        ws();
        if (i_ < t_.size() && t_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            ws();
            if (i_ >= t_.size() || t_[i_] != '"') return fail("expected a member name");
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i_ >= t_.size() || t_[i_] != ':') return fail("expected :");
            ++i_;
            Value v;
            if (!value(v, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            ws();
            if (i_ >= t_.size()) return fail("unterminated object");
            if (t_[i_] == ',') {
                ++i_;
                continue;
            }
            if (t_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("expected , or }");
        }
    }

    const std::string &t_;
    std::size_t i_ = 0;
};

} // namespace

const Value *Value::get(const std::string &key) const
{
    if (kind != Kind::Object) return nullptr;
    for (const auto &m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

bool Value::is_int() const
{
    if (kind != Kind::Number || text.empty()) return false;
    if (text.find_first_of(".eE") != std::string::npos) return false;
    errno = 0;
    char *end = nullptr;
    (void)std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0';
}

std::int64_t Value::as_int(std::int64_t def) const
{
    if (!is_int()) return def;
    return std::strtoll(text.c_str(), nullptr, 10);
}

bool Value::as_bool(bool def) const
{
    return kind == Kind::Bool ? boolean : def;
}

std::string Value::as_string(const std::string &def) const
{
    return kind == Kind::String ? text : def;
}

bool parse(const std::string &text, Value &out, std::string *why)
{
    out = Value();
    if (text.size() > kMaxText) {
        if (why) *why = "file too large";
        return false;
    }
    Parser p(text);
    Value v;
    if (!p.value(v, 0) || !p.end()) {
        if (why) *why = p.why;
        return false;
    }
    out = std::move(v);
    return true;
}

std::string quote(const std::string &s)
{
    static const char kHex[] = "0123456789abcdef";
    std::string q = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': q += "\\\""; break;
        case '\\': q += "\\\\"; break;
        case '\n': q += "\\n"; break;
        case '\r': q += "\\r"; break;
        case '\t': q += "\\t"; break;
        default:
            if (c < 0x20) {
                q += "\\u00";
                q += kHex[c >> 4];
                q += kHex[c & 15];
            } else {
                q += ch;
            }
        }
    }
    q += '"';
    return q;
}

} // namespace json
} // namespace klondike
