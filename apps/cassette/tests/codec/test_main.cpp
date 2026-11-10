/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: main(). The fakes always run; the real-library group runs only when libFLAC / libopusenc load
 * (LD_LIBRARY_PATH may point at a private root, tests/codec/run.sh) and prints "skipped" otherwise.
 */
#include "check.hpp"

int g_pass = 0, g_fail = 0, g_skip = 0;

int main() {
    test_sink();
    test_flac();
    test_opus();
    test_formats();
    test_real();
    std::printf("cassette codec tests: %d checks passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
