/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI tests: main().
 */
#include "check.hpp"

int g_pass = 0, g_fail = 0;

int main()
{
    test_format();
    test_meter_steps();
    test_wave();
    test_fit();
    test_load_anim();
    std::printf("cassette ui tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
