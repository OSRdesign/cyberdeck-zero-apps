/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core tests: the CHECK macros and the test groups (one binary, test-cassette).
 */
#pragma once

#include <cstdio>
#include <string>

extern int g_pass, g_fail;

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
template <typename T> std::string test_text(const T &v) { return std::to_string(v); }

void test_ring();
void test_wav();
void test_writer();
void test_journal();
void test_repair();
void test_crash();
void test_rates();
void test_stream_scan();
void test_codec_writer();
void test_codec_crash();
