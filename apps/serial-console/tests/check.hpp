/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console tests: the CHECK macro and the test groups (one binary, test-serial-console, built by
 * ../../_common/build_app.sh host from the .cpp files of tests/ and src/core/).
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

inline std::string test_text(const std::string &s)
{
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c >= 0x7F) {
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
template <typename T> std::string test_text(const T &v) { return std::to_string(v); }

/* A fresh empty folder for a test (removed by the test). */
std::string test_tmpdir(const char *tag);

void test_devices();
void test_ports();
void test_term();
void test_baud();
void test_settings();
void test_serial_pty();
void test_identify();
void test_bootproto();
void test_probe_pty();
