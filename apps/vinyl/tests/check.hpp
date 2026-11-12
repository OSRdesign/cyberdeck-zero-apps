/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the CHECK macros (Cassette's pattern) and the helpers the groups share. One binary, test-vinyl, built by
 * the headless harness (build.sh host) and by tests/Makefile (tests/run.sh, -Werror, sanitizers on request).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
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
inline std::string test_text(float v) { return std::to_string(v); }
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
std::string read_file(const std::string &path);

/* little helpers to build byte strings */
void put_le16(std::string &s, uint32_t v);
void put_le32(std::string &s, uint32_t v);
void put_be16(std::string &s, uint32_t v);
void put_be32(std::string &s, uint32_t v);

/* the test groups */
void test_tags();
void test_decoders();
void test_queue();
void test_browse();
void test_resume();
void test_media_keys();
void test_engine();
void test_outputs();
void test_launcher();
void test_art();
void test_layout();
void test_real_libs();
void test_transport();
void test_hits();
