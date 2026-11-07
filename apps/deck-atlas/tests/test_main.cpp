/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: main() and the shared helpers.
 */
#include "check.hpp"

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

int g_pass = 0, g_fail = 0;

std::string test_app_dir()
{
#ifdef CDK_APP_DIR
    return CDK_APP_DIR;
#else
    const char *e = std::getenv("DECKATLAS_APP_DIR");
    return e && *e ? e : "..";
#endif
}

std::string test_read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string test_temp_dir()
{
    const char *t = std::getenv("TMPDIR");
    std::string tmpl = std::string(t && *t ? t : "/tmp") + "/deck-atlas-test-XXXXXX";
    char *d = ::mkdtemp(&tmpl[0]);
    return d ? std::string(d) : std::string();
}

void test_remove_tree(const std::string &path)
{
    if (path.size() < 8) return; /* never "/" or a short path by mistake */
    if (DIR *d = ::opendir(path.c_str())) {
        while (struct dirent *e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string p = path + "/" + n;
            struct stat st;
            if (::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) test_remove_tree(p);
            else ::unlink(p.c_str());
        }
        ::closedir(d);
    }
    ::rmdir(path.c_str());
}

int main()
{
    test_settings();
    test_format();
    test_import();
    test_sources();
    test_browser();
    test_geo();
    test_attribution();
    test_nmea();
    test_gps_ids();
    test_gps_fix();
    test_gps_sky();
    test_gps_chip();
    test_gps_page();
    test_gps_link();
    test_gps_posix();
    test_gps_demo();
    test_follow();
    test_position_state();
    test_settings_gps();
    test_package();
    std::printf("deck-atlas core tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
