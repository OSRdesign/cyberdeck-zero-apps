/*
 * SPDX-License-Identifier: MIT
 */
#include "utf8.hpp"

namespace mdnotes {

namespace {

inline bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* Length of the valid sequence at s[i] (1..4), 0 when it is malformed. */
std::size_t seq_len(const std::string &s, std::size_t i, std::uint32_t *out)
{
    const std::size_t n = s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        if (i + 1 >= n || !cont(static_cast<unsigned char>(s[i + 1]))) return 0;
        *out = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (i + 2 >= n) return 0;
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]), c2 = static_cast<unsigned char>(s[i + 2]);
        if (!cont(c1) || !cont(c2)) return 0;
        if (c == 0xE0 && c1 < 0xA0) return 0;   /* overlong */
        if (c == 0xED && c1 >= 0xA0) return 0;  /* surrogate */
        *out = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (i + 3 >= n) return 0;
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]), c2 = static_cast<unsigned char>(s[i + 2]),
                            c3 = static_cast<unsigned char>(s[i + 3]);
        if (!cont(c1) || !cont(c2) || !cont(c3)) return 0;
        if (c == 0xF0 && c1 < 0x90) return 0;   /* overlong */
        if (c == 0xF4 && c1 >= 0x90) return 0;  /* above U+10FFFF */
        *out = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
        return 4;
    }
    return 0;
}

} // namespace

bool utf8_valid(const std::string &s)
{
    std::size_t i = 0;
    std::uint32_t cp;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const std::size_t n = seq_len(s, i, &cp);
        if (!n) return false;
        i += n;
    }
    return true;
}

std::uint32_t utf8_next(const std::string &s, std::size_t &i)
{
    std::uint32_t cp = 0xFFFD;
    const std::size_t n = seq_len(s, i, &cp);
    if (!n) {
        ++i;
        return 0xFFFD;
    }
    i += n;
    return cp;
}

void utf8_append(std::string &out, std::uint32_t cp)
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

std::size_t utf8_length(const std::string &s)
{
    std::size_t i = 0, n = 0;
    while (i < s.size()) {
        utf8_next(s, i);
        ++n;
    }
    return n;
}

std::uint32_t fold_cp(std::uint32_t c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;               /* Latin-1 */
    if (c >= 0x100 && c <= 0x137) return c | 1u;                          /* Latin Extended-A: even upper */
    if (c >= 0x139 && c <= 0x148) return (c & 1u) ? c + 1 : c;            /* odd upper */
    if (c >= 0x14A && c <= 0x177) return c | 1u;
    if (c == 0x178) return 0xFF;                                          /* Y with diaeresis */
    if (c >= 0x179 && c <= 0x17E) return (c & 1u) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;            /* Greek */
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;                          /* Cyrillic */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

std::string fold_case(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
            ++i;
            continue;
        }
        std::uint32_t cp;
        const std::size_t n = seq_len(s, i, &cp);
        if (!n) {
            out += s[i];   /* a malformed byte stays as it is */
            ++i;
            continue;
        }
        utf8_append(out, fold_cp(cp));
        i += n;
    }
    return out;
}

bool equal_ci(const std::string &a, const std::string &b)
{
    if (a.size() == b.size() && a == b) return true;
    return fold_case(a) == fold_case(b);
}

} // namespace mdnotes
