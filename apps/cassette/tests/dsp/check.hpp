/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette DSP tests: the CHECK macros (same pattern as tests/alsa) plus CHECK_NEAR, and the test groups (one binary).
 */
#pragma once

#include <cmath>
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

#define CHECK_NEAR(a, b, tol)                                                                                          \
    do {                                                                                                               \
        const double va_ = double(a);                                                                                  \
        const double vb_ = double(b);                                                                                  \
        if (std::fabs(va_ - vb_) <= double(tol)) ++g_pass;                                                             \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s, %s) failed: %.6f vs %.6f\n", __FILE__, __LINE__, #a, #b,   \
                         #tol, va_, vb_);                                                                              \
        }                                                                                                              \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
template <typename T> std::string test_text(const T &v) { return std::to_string(static_cast<long long>(v)); }

/* counting allocator (alloc_count.cpp): global operator new calls since start */
long long alloc_count();

void test_meter();
void test_gain();
void test_filters();
void test_sink();
void test_rates();
