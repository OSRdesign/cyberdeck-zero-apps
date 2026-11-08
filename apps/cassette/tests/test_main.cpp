/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core tests: main().
 */
#include "check.hpp"

int g_pass = 0, g_fail = 0;

int main() {
    test_ring();
    test_wav();
    test_writer();
    test_journal();
    test_repair();
    test_crash();
    std::printf("cassette core tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
