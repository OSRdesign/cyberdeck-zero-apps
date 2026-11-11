/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: main().
 */
#include "check.hpp"

int g_pass = 0, g_fail = 0;

int main() {
    test_peaks();
    test_trim();
    test_crash_steps();
    test_crash_fork();
    test_codec_edit();
    std::printf("cassette edit tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
