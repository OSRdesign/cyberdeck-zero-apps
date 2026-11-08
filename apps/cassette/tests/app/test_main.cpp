/* SPDX-License-Identifier: MIT */
#include "check.hpp"

#include <cstring>

int g_pass = 0, g_fail = 0, g_skip = 0;

int main(int argc, char **argv) {
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0; /* leaves out the slow real-time takes */
    test_faults();   /* first: it forks, before any other thread exists */
    test_record();
    test_settings();
    test_unplug();
    if (!quick) test_deck();
    test_edit();
    std::printf("cassette-app tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
