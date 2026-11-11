/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound-server tests: main(). Everything runs on the fake server, except the last group, which loads the real
 * libpulse when it is there (LD_LIBRARY_PATH may point at a private root, tests/pulse/run.sh) and prints "skipped"
 * otherwise. The real smoke test against a running server is real_pulse_test.cpp (its own binary).
 */
#include "check.hpp"

int g_pass = 0, g_fail = 0, g_skip = 0;

int main() {
    test_lib();
    test_list();
    test_capture();
    test_silence();
    test_playback();
    test_threads();
    test_pcm();
    test_bt_input();
    test_bt_output();
    test_real_lib();
    std::printf("cassette-pulse tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
