/*
 * SPDX-License-Identifier: MIT
 */
#include "util.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace rssreader {

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

bool starts_with(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool iequals(const std::string &a, const std::string &b) { return a.size() == b.size() && lower(a) == lower(b); }
bool icontains(const std::string &hay, const std::string &needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

void append_utf8(std::string &out, std::uint32_t cp)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
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

/* Length of the valid UTF-8 sequence at s[i], or 0. */
static std::size_t utf8_len_at(const std::string &s, std::size_t i)
{
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t need;
    std::uint32_t cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { need = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 4; cp = c & 0x07; }
    else return 0;
    if (i + need > s.size()) return 0;
    for (std::size_t k = 1; k < need; ++k) {
        unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if (need == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (need == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return need;
}

std::string utf8_sanitize(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        std::size_t n = utf8_len_at(s, i);
        if (n == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s, i, n);
            i += n;
        }
    }
    return out;
}

std::string truncate_utf8(const std::string &s, std::size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

static const std::uint16_t kCp1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                          0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                          0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

std::string latin1_to_utf8(const std::string &s, bool cp1252, bool latin9)
{
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char ch : s) {
        std::uint32_t c = static_cast<unsigned char>(ch);
        if (cp1252 && c >= 0x80 && c <= 0x9F) c = kCp1252[c - 0x80];
        if (latin9) {
            switch (c) {
            case 0xA4: c = 0x20AC; break;
            case 0xA6: c = 0x160; break;
            case 0xA8: c = 0x161; break;
            case 0xB4: c = 0x17D; break;
            case 0xB8: c = 0x17E; break;
            case 0xBC: c = 0x152; break;
            case 0xBD: c = 0x153; break;
            case 0xBE: c = 0x178; break;
            default: break;
            }
        }
        append_utf8(out, c);
    }
    return out;
}

static const std::unordered_map<std::string, std::uint32_t> &entity_table()
{
    static const std::unordered_map<std::string, std::uint32_t> table = [] {
        std::unordered_map<std::string, std::uint32_t> t;
        static const char *latin1[96] = {
            "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo", "not", "shy", "reg",
            "macr", "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14",
            "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave",
            "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde",
            "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc",
            "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
            "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml",
            "yacute", "thorn", "yuml"};
        for (int i = 0; i < 96; ++i) t[latin1[i]] = static_cast<std::uint32_t>(160 + i);
        struct E { const char *n; std::uint32_t c; };
        static const E extra[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", 0x27}, {"ndash", 0x2013},
            {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
            {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030},
            {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"euro", 0x20AC}, {"trade", 0x2122},
            {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193}, {"ensp", 0x2002}, {"emsp", 0x2003},
            {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F}, {"OElig", 0x152},
            {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"fnof", 0x192}, {"circ", 0x2C6},
            {"tilde", 0x2DC}, {"infin", 0x221E}, {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265}, {"hearts", 0x2665},
            {"check", 0x2713}, {"minus", 0x2212}, {"hyphen", 0x2010}};
        for (const E &e : extra) t[e.n] = e.c;
        return t;
    }();
    return table;
}

std::string decode_entities(const std::string &s)
{
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        std::size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12 || semi == i + 1) { out += '&'; continue; }
        std::string name = s.substr(i + 1, semi - i - 1);
        std::uint32_t cp = 0;
        bool ok = false;
        if (name[0] == '#') {
            bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            std::size_t k = hex ? 2 : 1;
            if (k < name.size()) {
                ok = true;
                for (; k < name.size(); ++k) {
                    char c = name[k];
                    int d = -1;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    if (d < 0) { ok = false; break; }
                    cp = cp * (hex ? 16 : 10) + static_cast<std::uint32_t>(d);
                    if (cp > 0x10FFFF) cp = 0x110000;
                }
            }
            if (ok) {
                if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
                if (cp >= 0x80 && cp <= 0x9F) cp = kCp1252[cp - 0x80];
            }
        } else {
            auto it = entity_table().find(name);
            if (it != entity_table().end()) { cp = it->second; ok = true; }
        }
        if (!ok) { out += '&'; continue; }
        append_utf8(out, cp);
        i = semi;
    }
    return out;
}

std::string html_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string collapse_ws(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool pending = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool space = false;
        std::size_t skip = 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            space = true;
        } else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            space = true;
            skip = 1;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            unsigned char d = static_cast<unsigned char>(s[i + 2]);
            if ((d >= 0x80 && d <= 0x8A) || d == 0xAF) { space = true; skip = 2; }           /* U+2000..200A, U+202F */
            else if (d >= 0x8B && d <= 0x8D) { i += 2; continue; }                            /* zero-width: dropped */
        } else if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBB &&
                   static_cast<unsigned char>(s[i + 2]) == 0xBF) {
            i += 2;                                                                          /* BOM / ZWNBSP */
            continue;
        }
        if (space) {
            pending = true;
            i += skip;
            continue;
        }
        if (pending && !out.empty()) out += ' ';
        pending = false;
        out += static_cast<char>(c);
    }
    return out;
}

std::string fold_ascii(const std::string &s)
{
    static const char *map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x80 && d <= 0xBF) {
                char m = map[d - 0x80];
                out += (m >= 'A' && m <= 'Z') ? static_cast<char>(m + 32) : m;
                ++i;
                continue;
            }
        }
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
    }
    return out;
}

std::string hash16(const std::string &s)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string safe_name(const std::string &s)
{
    std::string out;
    for (char c : s) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
        if (out.size() >= 96) break;
    }
    if (out.empty() || out[0] == '.') out.insert(out.begin(), '_');
    return out;
}

} // namespace rssreader
