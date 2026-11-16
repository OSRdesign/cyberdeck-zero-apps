/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the runner and the shared helpers (check.hpp).
 */
#include "check.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int g_pass = 0, g_fail = 0, g_skip = 0;

std::string make_temp_dir(const char *tag)
{
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/vhs-" + tag + "-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    const char *d = mkdtemp(buf.data());
    return d ? std::string(d) : std::string();
}

void rm_tree(const std::string &dir)
{
    if (dir.size() < 10 || dir.find("vhs-") == std::string::npos) return;
    const std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", dir.c_str());
}

bool write_file(const std::string &path, const std::string &bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

void make_dirs(const std::string &path)
{
    std::string cur;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < path.size()) cur += path[i];
    }
}

bool wait_for(int ms, const std::function<bool()> &pred)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

std::string fixture(const char *name)
{
    const char *dir = std::getenv("VHS_FIXTURES");
    if (!dir || !dir[0]) return "";
    const std::string p = std::string(dir) + "/" + name;
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? p : std::string();
}

int main(int argc, char **argv)
{
    struct Group {
        const char *name;
        void (*fn)();
    } groups[] = {{"mailbox", test_mailbox},     {"modes", test_modes},         {"convert", test_convert},
                  {"layout", test_layout},       {"stats", test_stats},         {"v4l2", test_v4l2},
                  {"controls", test_controls},   {"capture", test_capture},     {"discovery", test_discovery},
                  {"select", test_select},       {"settings", test_settings},   {"pipeline", test_pipeline},
                  {"turbojpeg", test_turbojpeg}, {"cli", test_cli},             {"controller", test_controller},
                  {"jpeg", test_jpeg},           {"naming", test_naming},       {"photo", test_photo},
                  {"record", test_record},       {"recorder", test_recorder},   {"recapp", test_rec_app},
                  {"scenes", test_scenes},       {"movie", test_movie},         {"playaudio", test_play_audio},
                  {"drives", test_drives}};
    for (const Group &g : groups) {
        if (argc > 1) {
            bool wanted = false;
            for (int i = 1; i < argc; ++i) wanted |= std::strcmp(argv[i], g.name) == 0;
            if (!wanted) continue;
        }
        const int before = g_fail;
        g.fn();
        std::fprintf(stderr, "group %-10s %s\n", g.name, g_fail == before ? "ok" : "FAILED");
    }
    std::printf("vhs tests: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
