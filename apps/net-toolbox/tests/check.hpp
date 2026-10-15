/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the CHECK macros, the fixture helpers and the test groups (one binary, test-net-toolbox, built by
 * ../../_common/build_app.sh host from the .cpp files of tests/ and src/core/).
 */
#pragma once

#include <cmath>
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

#define CHECK_NEAR(a, b, eps)                                                                                          \
    do {                                                                                                               \
        const double va_ = (a);                                                                                        \
        const double vb_ = (b);                                                                                        \
        if (std::fabs(va_ - vb_) <= (eps)) ++g_pass;                                                                   \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                                              \
    } while (0)

/* a check that cannot run on this host: said once, counted apart */
#define SKIP(why)                                                                                                      \
    do {                                                                                                               \
        ++g_skip;                                                                                                      \
        std::printf("SKIP %s:%d: %s\n", __FILE__, __LINE__, why);                                                       \
    } while (0)

inline std::string test_text(const std::string &s)
{
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}
inline std::string test_text(const char *s) { return test_text(std::string(s ? s : "")); }
inline std::string test_text(bool v) { return v ? "true" : "false"; }
template <typename T> std::string test_text(const T &v) { return std::to_string(v); }

/* tests/fixtures/<board> */
std::string fixture_dir(const char *board);
/* a fixture file's text ("" when missing) */
std::string fixture(const char *board, const char *path);
/* a fresh empty folder for a test */
std::string test_tmpdir(const char *tag);
/* removes a folder and everything in it */
void remove_dir(const std::string &dir);

void test_text_helpers();
void test_netaddr();
void test_interfaces();
void test_dns_wire();
void test_dns_fuzz();
void test_dns_client();
void test_ping_stats();
void test_ping_engine();
void test_ping_real();
void test_portcheck();
void test_http();
void test_diagnosis();
void test_netlog();
void test_settings();
void test_worker();
void test_ui_model();
