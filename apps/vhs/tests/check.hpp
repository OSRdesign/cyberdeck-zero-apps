/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the CHECK macros (Cassette's and Vinyl's pattern) and the helpers the groups share. One binary, test-vhs,
 * built by the headless harness (build.sh host) and by tests/Makefile (tests/run.sh: -Werror, sanitizers on request,
 * the real libturbojpeg and an ffmpeg-made MJPEG fixture when the build box has them).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

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
        const double va_ = static_cast<double>(a);                                                                     \
        const double vb_ = static_cast<double>(b);                                                                     \
        if (std::fabs(va_ - vb_) <= (tol)) ++g_pass;                                                                   \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, va_, vb_);  \
        }                                                                                                              \
    } while (0)

#define SKIP(why)                                                                                                      \
    do {                                                                                                               \
        ++g_skip;                                                                                                      \
        std::fprintf(stderr, "skip: %s\n", why);                                                                       \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
inline std::string test_text(double v) { return std::to_string(v); }
inline std::string test_text(int v) { return std::to_string(v); }
inline std::string test_text(long v) { return std::to_string(v); }
inline std::string test_text(long long v) { return std::to_string(v); }
inline std::string test_text(unsigned v) { return std::to_string(v); }
inline std::string test_text(unsigned long v) { return std::to_string(v); }
inline std::string test_text(unsigned long long v) { return std::to_string(v); }
template <typename T> std::string test_text(const T &) { return "?"; }

/* a fresh temporary folder (removed by rm_tree) */
std::string make_temp_dir(const char *tag);
void rm_tree(const std::string &dir);
bool write_file(const std::string &path, const std::string &bytes);
void make_dirs(const std::string &path);
/* waits up to ms for pred() (polling every 5 ms) */
bool wait_for(int ms, const std::function<bool()> &pred);
/* $VHS_FIXTURES/name when that file exists, else "" */
std::string fixture(const char *name);

/* the test groups */
void test_mailbox();
void test_modes();
void test_convert();
void test_layout();
void test_stats();
void test_v4l2();
void test_controls();
void test_capture();
void test_discovery();
void test_select();
void test_settings();
void test_pipeline();
void test_turbojpeg();
void test_cli();
void test_controller();
void test_jpeg();
void test_naming();
void test_photo();
void test_record();
void test_recorder();
void test_rec_app();
