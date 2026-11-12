/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: main().
 */
#include "check.hpp"

#include <cstdlib>
#include <ctime>

int g_pass = 0, g_fail = 0;

int main() {
    setenv("TZ", "UTC", 1); /* datetime_from_wall() uses local time */
    tzset();
    test_json();
    test_naming();
    test_counter();
    test_edits();
    test_take_number_setting();
    test_recordings();
    test_rename_prefixes();
    test_settings();
    test_settings_bt();
    test_formats_takes();
    std::printf("cassette takes tests: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
