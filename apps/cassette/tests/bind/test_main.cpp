/* SPDX-License-Identifier: MIT */
#include "check.hpp"

#include <cstring>

int g_pass = 0, g_fail = 0, g_skip = 0;

int main(int argc, char **argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0; /* leaves out the real-time end-to-end runs */
    test_map();
    test_bt_map();
    test_worker();
    test_binder();
    if (!quick) test_demo();
    std::printf("cassette-bind tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
