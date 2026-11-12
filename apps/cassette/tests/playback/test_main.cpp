/* SPDX-License-Identifier: MIT */
#include "check.hpp"

#include <cstring>

int g_pass = 0, g_fail = 0, g_skip = 0;

int main(int argc, char **argv) {
    const bool threads_only = argc > 1 && std::strcmp(argv[1], "--threads") == 0;
    if (!threads_only) {
        test_reader();
        test_player();
        test_scan();
        test_alsa_sink();
        test_output();
        test_countdown();
        test_deck();
        test_rates();
        test_monitor();
        test_codec_play();
        test_pulse_sink();
        test_pulse_monitor();
    }
    test_threads();
    std::printf("cassette-playback tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
