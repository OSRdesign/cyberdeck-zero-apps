/* SPDX-License-Identifier: MIT */
#include "check.hpp"

int g_pass = 0, g_fail = 0;

int main() {
    test_meter();
    test_gain();
    test_filters();
    test_sink();
    test_rates();
    std::printf("cassette-dsp tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
