/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests (task T6a): the CHECK macros (the pattern of tests/alsa/check.hpp) and the test groups (one binary).
 */
#pragma once

#include <chrono>
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

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
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










void test_record();      /* tone through fake ALSA to a WAV and back, idle meter, splits, no level control */
void test_deck();        /* record timer, pause, scan, jump */
void test_unplug();      /* unplug mid-take, replug, card at another index */
void test_edit();        /* trim and splits, replace, app leave during a save, in-memory peaks */
void test_faults();      /* abandoned take and repair, disk full, low space */
void test_notes();       /* the capture path on screen: server notice, other-apps notice, lost audio (study 058) */
void test_settings();    /* persistence across a restart */
