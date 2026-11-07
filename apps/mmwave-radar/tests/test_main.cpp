/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar tests: main() and the shared helpers.
 */
#include "check.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef MMW_DATA_DIR
#ifdef CDK_APP_DIR /* the headless Makefile (build.sh host) */
#define MMW_DATA_DIR CDK_APP_DIR "/tests/data/"
#else
#define MMW_DATA_DIR "tests/data/"
#endif
#endif

int g_pass = 0, g_fail = 0;

std::string test_data_dir()
{
    const char *e = std::getenv("MMW_DATA_DIR");
    std::string d = e && e[0] ? e : MMW_DATA_DIR;
    if (!d.empty() && d.back() != '/') d += '/';
    return d;
}

std::string test_read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main()
{
    test_frame();
    test_units();
    test_detect();
    test_tracker();
    test_link();
    test_posix();
    test_view();
    test_package();
    test_ghost();
    test_driver();
    test_ld2451();
    test_ble();
    std::printf("mmwave-radar core tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
