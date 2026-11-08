/* SPDX-License-Identifier: MIT */
#include "check.hpp"

int g_pass = 0, g_fail = 0, g_skip = 0;

int main() {
    test_card();
    test_open();
    test_capture();
    test_mixer();
    test_real();
    std::printf("cassette-alsa tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
