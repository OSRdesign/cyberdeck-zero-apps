/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: the CHECK macros (the pattern of tests/alsa/check.hpp, with skips) and the test groups.
 */
#pragma once

#include <cstdio>
#include <string>

extern int g_pass, g_fail, g_skip;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        const auto va_ = (a);                                                                                          \
        const auto vb_ = (b);                                                                                          \
        if (va_ == vb_) ++g_pass;                                                                                      \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: [%s] vs [%s]\n", __FILE__, __LINE__, #a, #b,          \
                         test_text(va_).c_str(), test_text(vb_).c_str());                                              \
        }                                                                                                              \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
inline std::string test_text(double v) { return std::to_string(v); }
template <typename T> std::string test_text(const T &v) { return std::to_string(static_cast<long long>(v)); }

#define SKIP(why)                                                                                                      \
    do {                                                                                                               \
        ++g_skip;                                                                                                      \
        std::printf("  skipped: %s\n", why);                                                                           \
    } while (0)

void test_sink();
void test_flac();
void test_opus();
void test_formats();
void test_real();
void test_writer_real();
void test_decoders();
void test_decoders_real();
void test_merge_real();
