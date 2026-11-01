/*
 * SPDX-License-Identifier: MIT
 *
 * Shared helpers of the unit tests (test_core.cpp has main(); the other files add their groups). Every .cpp file of
 * tests/ is linked with src/core into one binary, test-home-assistant, by ../../_common/build_app.sh host.
 */
#pragma once

#include <cstdio>
#include <string>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

inline int g_pass = 0;
inline int g_fail = 0;

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
        const auto check_a_ = (a); /* copies: (a) may be a member of a temporary */                                   \
        const auto check_b_ = (b);                                                                                     \
        if (check_a_ == check_b_) ++g_pass;                                                                            \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed:\n  got      [%s]\n  expected [%s]\n", __FILE__,      \
                         __LINE__, #a, #b, test_str(check_a_).c_str(), test_str(check_b_).c_str());                   \
        }                                                                                                              \
    } while (0)

inline std::string test_str(const std::string &s) { return s; }
inline std::string test_str(const char *s) { return s ? s : "(null)"; }
inline std::string test_str(long long v) { return std::to_string(v); }
inline std::string test_str(long v) { return std::to_string(v); }
inline std::string test_str(int v) { return std::to_string(v); }
inline std::string test_str(unsigned long v) { return std::to_string(v); }
inline std::string test_str(unsigned v) { return std::to_string(v); }
inline std::string test_str(bool v) { return v ? "true" : "false"; }
inline std::string test_str(double v) { return std::to_string(v); }

void test_json_group();
void test_text_group();
void test_model_group();
void test_format_group();
void test_engine_group();
void test_sim_group();
void test_store_group();
void test_qr_group();
void test_package_group();
