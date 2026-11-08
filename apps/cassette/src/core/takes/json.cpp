/*
 * SPDX-License-Identifier: MIT
 */
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cassette {
namespace json {

Value Value::boolean(bool v) {
    Value r;
    r.type = Bool;
    r.b = v;
    return r;
}
Value Value::number(double v) {
    Value r;
    r.type = Num;
    r.n = v;
    return r;
}
Value Value::string(const std::string &v) {
    Value r;
    r.type = Str;
    r.s = v;
    return r;
}
Value Value::object() {
    Value r;
    r.type = Obj;
    return r;
}

const Value *Value::get(const std::string &key) const {
    if (type != Obj) return nullptr;
    for (const auto &kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
Value *Value::get(const std::string &key) {
    if (type != Obj) return nullptr;
    for (auto &kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}
Value &Value::set(const std::string &key, Value v) {
    if (type != Obj) *this = object();
    for (auto &kv : obj)
        if (kv.first == key) {
            kv.second = std::move(v);
            return kv.second;
        }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}
bool Value::erase(const std::string &key) {
    if (type != Obj) return false;
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (it->first == key) {
            obj.erase(it);
            return true;
        }
    return false;
}

namespace {

constexpr int kMaxDepth = 32;

struct Parser {
    const std::string &s;
    size_t p = 0;
    bool bad = false;
    explicit Parser(const std::string &t) : s(t) {}

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
    static void utf8(std::string &o, unsigned cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned &v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    std::string str() {
        std::string r;
        if (!eat('"')) {
            bad = true;
            return r;
        }
        while (p < s.size() && s[p] != '"') {
            unsigned char c = static_cast<unsigned char>(s[p++]);
            if (c < 0x20) {
                bad = true;
                return r;
            }
            if (c != '\\') {
                r += static_cast<char>(c);
                continue;
            }
            if (p >= s.size()) break;
            char e = s[p++];
            switch (e) {
            case '"': r += '"'; break;
            case '\\': r += '\\'; break;
            case '/': r += '/'; break;
            case 'b': r += '\b'; break;
            case 'f': r += '\f'; break;
            case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;
            case 't': r += '\t'; break;
            case 'u': {
                unsigned v = 0;
                if (!hex4(v)) {
                    bad = true;
                    return r;
                }
                if (v >= 0xD800 && v < 0xDC00) {
                    unsigned lo = 0;
                    if (p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000) {
                            bad = true;
                            return r;
                        }
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        v = 0xFFFD;
                    }
                } else if (v >= 0xDC00 && v < 0xE000) {
                    v = 0xFFFD;
                }
                utf8(r, v);
                break;
            }
            default: bad = true; return r;
            }
        }
        if (p >= s.size()) bad = true;
        else p++;
        return r;
    }
    Value value(int depth) {
        Value v;
        ws();
        if (p >= s.size() || depth > kMaxDepth) {
            bad = true;
            return v;
        }
        char c = s[p];
        if (c == '{') {
            p++;
            v.type = Value::Obj;
            if (eat('}')) return v;
            do {
                ws();
                std::string k = str();
                if (bad || !eat(':')) {
                    bad = true;
                    return v;
                }
                Value child = value(depth + 1);
                if (bad) return v;
                v.set(k, std::move(child));
            } while (eat(','));
            if (!eat('}')) bad = true;
        } else if (c == '[') {
            p++;
            v.type = Value::Arr;
            if (eat(']')) return v;
            do {
                v.arr.push_back(value(depth + 1));
                if (bad) return v;
            } while (eat(','));
            if (!eat(']')) bad = true;
        } else if (c == '"') {
            v.type = Value::Str;
            v.s = str();
        } else if (s.compare(p, 4, "true") == 0) {
            v.type = Value::Bool, v.b = true, p += 4;
        } else if (s.compare(p, 5, "false") == 0) {
            v.type = Value::Bool, v.b = false, p += 5;
        } else if (s.compare(p, 4, "null") == 0) {
            p += 4;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const char *start = s.c_str() + p;
            char *end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start || !std::isfinite(d)) {
                bad = true;
                return v;
            }
            v.type = Value::Num;
            v.n = d;
            p += static_cast<size_t>(end - start);
        } else {
            bad = true;
        }
        return v;
    }
};

void esc(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    o += '"';
}

void num(std::string &o, double d) {
    if (!std::isfinite(d)) {
        o += "null";
        return;
    }
    char b[40];
    if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(b, sizeof b, "%.0f", d);
    else std::snprintf(b, sizeof b, "%.17g", d);
    o += b;
}

void emit(std::string &o, const Value &v, int ind) {
    const std::string pad(static_cast<size_t>(ind + 2), ' ');
    switch (v.type) {
    case Value::Null: o += "null"; break;
    case Value::Bool: o += v.b ? "true" : "false"; break;
    case Value::Num: num(o, v.n); break;
    case Value::Str: esc(o, v.s); break;
    case Value::Arr:
        if (v.arr.empty()) {
            o += "[]";
            break;
        }
        o += "[\n";
        for (size_t k = 0; k < v.arr.size(); k++) {
            o += pad;
            emit(o, v.arr[k], ind + 2);
            o += k + 1 < v.arr.size() ? ",\n" : "\n";
        }
        o += std::string(static_cast<size_t>(ind), ' ') + "]";
        break;
    case Value::Obj:
        if (v.obj.empty()) {
            o += "{}";
            break;
        }
        o += "{\n";
        for (size_t k = 0; k < v.obj.size(); k++) {
            o += pad;
            esc(o, v.obj[k].first);
            o += ": ";
            emit(o, v.obj[k].second, ind + 2);
            o += k + 1 < v.obj.size() ? ",\n" : "\n";
        }
        o += std::string(static_cast<size_t>(ind), ' ') + "}";
        break;
    }
}

}  // namespace

bool parse(const std::string &text, Value &out) {
    Parser ps(text);
    Value v = ps.value(0);
    ps.ws();
    if (ps.bad || ps.p != text.size()) {
        out = Value();
        return false;
    }
    out = std::move(v);
    return true;
}

std::string write(const Value &v) {
    std::string o;
    emit(o, v, 0);
    o += '\n';
    return o;
}

}  // namespace json
}  // namespace cassette
