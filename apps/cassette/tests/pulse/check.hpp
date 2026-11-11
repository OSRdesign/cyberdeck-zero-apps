/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound-server tests: the CHECK macros (the same pattern as tests/alsa) and the test groups (one binary).
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

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

#define CHECK_NEAR(a, b, tol)                                                                                          \
    do {                                                                                                               \
        const double va_ = (a);                                                                                        \
        const double vb_ = (b);                                                                                        \
        if (std::fabs(va_ - vb_) <= (tol)) ++g_pass;                                                                   \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, va_, vb_);  \
        }                                                                                                              \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
inline std::string test_text(double v) { return std::to_string(v); }
template <typename T> std::string test_text(const T &v) { return std::to_string(static_cast<long long>(v)); }

/* Polls `pred` until true or `ms` elapse; returns the final value. */
template <typename F> bool wait_until(F pred, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return pred();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void test_lib();
void test_list();
void test_capture();
void test_silence();
void test_playback();
void test_threads();
void test_pcm();
void test_real_lib();
