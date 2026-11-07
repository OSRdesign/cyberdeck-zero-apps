/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the CHECK macros and the test groups (one binary, test-deck-atlas, built and run by build.sh host).
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

#define CHECK_NEAR(a, b, eps)                                                                                          \
    do {                                                                                                               \
        const double va_ = (a), vb_ = (b);                                                                             \
        if (va_ - vb_ <= (eps) && vb_ - va_ <= (eps)) ++g_pass;                                                        \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %.6f vs %.6f\n", __FILE__, __LINE__, #a, #b, va_,   \
                         vb_);                                                                                         \
        }                                                                                                              \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
inline std::string test_text(bool b) { return b ? "true" : "false"; }
template <typename T> std::string test_text(const T &v) { return std::to_string(v); }

/* The app folder (CDK_APP_DIR from the headless Makefile). */
std::string test_app_dir();
/* A whole file ("" when missing). */
std::string test_read_file(const std::string &path);
/* A new empty folder under $TMPDIR (removed by the test that made it). */
std::string test_temp_dir();
void test_remove_tree(const std::string &path);

void test_settings();
void test_format();
void test_import();
void test_sources();
void test_browser();
void test_geo();
void test_attribution();
void test_nmea();
void test_gps_ids();
void test_gps_fix();
void test_gps_sky();
void test_gps_chip();
void test_gps_link();
void test_gps_posix();
void test_gps_demo();
void test_follow();
void test_position_state();
void test_settings_gps();
void test_package();
