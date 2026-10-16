/*
 * SPDX-License-Identifier: MIT
 */

#include "util.hpp"

#include <cstdio>
#include <ctime>

namespace meshzero {

namespace {
uint32_t (*g_time_source)() = nullptr;
}

std::string to_hex(const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool from_hex(const std::string &hex, uint8_t *out, size_t out_len)
{
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = hex_value(hex[2 * i]);
        const int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

/* Length of the valid UTF-8 sequence starting at s[i], or 0 when it is invalid. */
static size_t utf8_sequence(const std::string &s, size_t i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    uint32_t cp = 0;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return n;
}

std::string sanitize_utf8(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0) { ++i; continue; }
        if (c < 0x20 && c != '\n') { out.push_back('?'); ++i; continue; }
        if (c == 0x7F) { out.push_back('?'); ++i; continue; }
        const size_t n = utf8_sequence(in, i);
        if (n == 0) { out.push_back('?'); ++i; continue; }
        out.append(in, i, n);
        i += n;
    }
    return out;
}

std::string truncate_utf8(const std::string &s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

size_t utf8_length(const std::string &s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

namespace { int64_t g_offset = 0; }

uint32_t deck_unix()
{
    if (g_time_source) return g_time_source();
    return static_cast<uint32_t>(std::time(nullptr));
}

uint32_t now_unix()
{
    const int64_t t = static_cast<int64_t>(deck_unix()) + g_offset;
    return t < 0 ? 0 : static_cast<uint32_t>(t);
}

void set_time_offset(int64_t seconds)
{
    g_offset = seconds;
}

int64_t time_offset()
{
    return g_offset;
}

void set_time_source(uint32_t (*fn)())
{
    g_time_source = fn;
}

std::string format_age(int64_t seconds)
{
    char buf[32];
    if (seconds < 0) seconds = 0;
    if (seconds < 60) std::snprintf(buf, sizeof(buf), "%d s", static_cast<int>(seconds));
    else if (seconds < 3600) std::snprintf(buf, sizeof(buf), "%d min", static_cast<int>(seconds / 60));
    else if (seconds < 86400) std::snprintf(buf, sizeof(buf), "%d h", static_cast<int>(seconds / 3600));
    else std::snprintf(buf, sizeof(buf), "%d d", static_cast<int>(seconds / 86400));
    return buf;
}

} // namespace meshzero
