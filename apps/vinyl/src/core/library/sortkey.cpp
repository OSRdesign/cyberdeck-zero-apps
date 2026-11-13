/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl library: sort keys (see sortkey.hpp).
 */
#include "sortkey.hpp"

#include <cstdint>

namespace vinyl {

const char *const kUnknownSortKey = "\xEF\xBF\xBF";

namespace {

/* U+00C0-U+017F: the base letter of each (lower case); '-' keeps the character, upper-case letters stand for two:
 * A = "ae", T = "th", S = "ss", I = "ij", O = "oe" */
const char kLatin[] =
    "aaaaaaAceeeeiiii"  /* 00C0 */
    "dnooooo-ouuuuyTS"  /* 00D0 */
    "aaaaaaAceeeeiiii"  /* 00E0 */
    "dnooooo-ouuuuyTy"  /* 00F0 */
    "aaaaaaccccccccdd"  /* 0100 */
    "ddeeeeeeeeeegggg"  /* 0110 */
    "gggghhhhiiiiiiii"  /* 0120 */
    "iiIIjjkkklllllll"  /* 0130 */
    "lllnnnnnnnnnoooo"  /* 0140 */
    "ooOOrrrrrrssssss"  /* 0150 */
    "ssttttttuuuuuuuu"  /* 0160 */
    "uuuuwwyyyzzzzzzs"; /* 0170 */
static_assert(sizeof(kLatin) == 0xC0 + 1, "one letter for each of U+00C0-U+017F");

/* the next code point of s at i (an invalid byte is read as Latin-1, as clean_utf8 does) */
uint32_t next_cp(const std::string &s, size_t &i) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
    const size_t n = s.size();
    const unsigned c = p[i];
    const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = len > 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
    if (!ok) {
        ++i;
        return c;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (p[i + k] & 0x3F);
    i += len;
    return cp;
}

void put_cp(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) {
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

/* the folded form of one code point into o (nothing for a dropped one) */
void fold_cp(std::string &o, uint32_t cp) {
    if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0; /* full-width ASCII */
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        o += static_cast<char>(cp);
        return;
    }
    if (cp >= 0x300 && cp <= 0x36F) return; /* combining marks */
    if (cp >= 0xC0 && cp <= 0x17F) {
        const char b = kLatin[cp - 0xC0];
        switch (b) {
        case '-': put_cp(o, cp); return;
        case 'A': o += "ae"; return;
        case 'T': o += "th"; return;
        case 'S': o += "ss"; return;
        case 'I': o += "ij"; return;
        case 'O': o += "oe"; return;
        default: o += b; return;
        }
    }
    if (cp == 0x218 || cp == 0x219) { /* Romanian comma below */
        o += 's';
        return;
    }
    if (cp == 0x21A || cp == 0x21B) {
        o += 't';
        return;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) cp += 0x20; /* Greek capitals */
    else if (cp == 0x3C2) cp = 0x3C3;                            /* final sigma */
    else if (cp >= 0x410 && cp <= 0x42F) cp += 0x20;             /* Cyrillic capitals */
    else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;
    else if ((cp >= 0x30A1 && cp <= 0x30F6) || cp == 0x30FD || cp == 0x30FE) cp -= 0x60; /* katakana to hiragana */
    put_cp(o, cp);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum_ascii(unsigned char c) { return is_digit(static_cast<char>(c)) || (c >= 'a' && c <= 'z'); }

constexpr size_t kNumberWidth = 8;

}  // namespace

std::string fold_text(const std::string &text) {
    std::string o;
    o.reserve(text.size());
    bool space = false;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = next_cp(text, i);
        if (cp < 0x20 || cp == 0x7F || cp == ' ' || cp == 0xA0 || cp == 0x3000) { /* controls and spaces */
            space = !o.empty();
            continue;
        }
        if (space) o += ' ';
        space = false;
        fold_cp(o, cp);
    }
    return o;
}

std::string sort_key(const std::string &text) {
    const std::string f = fold_text(text);
    /* leading punctuation (ASCII only: a CJK name starts with its own characters) */
    size_t a = 0;
    while (a < f.size() && static_cast<unsigned char>(f[a]) < 0x80 && !is_alnum_ascii(static_cast<unsigned char>(f[a]))) ++a;
    if (a == f.size()) a = 0; /* nothing but punctuation: keep it as it is */
    std::string s = f.substr(a);
    std::string article;
    if (s.size() > 4 && s.compare(0, 4, "the ") == 0) {
        article = "the";
        s.erase(0, 4);
    }
    std::string o;
    o.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size();) {
        if (!is_digit(s[i])) {
            o += s[i++];
            continue;
        }
        size_t j = i;
        while (j < s.size() && is_digit(s[j])) ++j;
        size_t z = i;
        while (z + 1 < j && s[z] == '0') ++z; /* leading zeros (one kept for a plain 0) */
        const size_t len = j - z;
        if (len <= kNumberWidth) o.append(kNumberWidth - len, '0');
        o.append(s, z, len);
        i = j;
    }
    if (!article.empty()) {
        o += '\x01'; /* below every printable character: right after the same name without the article */
        o += article;
    }
    if (o.empty()) return kUnknownSortKey;
    return o;
}

size_t utf8_length(const std::string &text) {
    size_t n = 0;
    for (size_t i = 0; i < text.size(); ++n) next_cp(text, i);
    return n;
}

void prefix_range(const std::string &query, std::string &lo, std::string &hi) {
    lo.clear();
    hi.clear();
    if (fold_text(query).empty()) return;
    lo = sort_key(query);
    if (lo == kUnknownSortKey) {
        lo.clear();
        return;
    }
    hi = lo + '\xFF';
}

}  // namespace vinyl
