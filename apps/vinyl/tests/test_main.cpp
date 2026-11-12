/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the runner and the shared helpers.
 */
#include "check.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ftw.h>
#include <sstream>
#include <unistd.h>

int g_pass = 0, g_fail = 0, g_skip = 0;

std::string make_temp_dir(const char *tag) {
    const char *t = std::getenv("TMPDIR");
    std::string tmpl = std::string(t && *t ? t : "/tmp") + "/vinyl-test-" + tag + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const char *d = ::mkdtemp(buf.data());
    return d ? d : "";
}

namespace {
int rm_cb(const char *p, const struct stat *, int, struct FTW *) { return ::remove(p); }
}  // namespace

void rm_tree(const std::string &dir) {
    if (dir.find("/vinyl-test-") == std::string::npos) return;
    ::nftw(dir.c_str(), rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

bool write_file(const std::string &path, const std::string &bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void put_le16(std::string &s, uint32_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
void put_le32(std::string &s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void put_be16(std::string &s, uint32_t v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}
void put_be32(std::string &s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    struct Group {
        const char *name;
        void (*fn)();
    };
    const Group groups[] = {
        {"tags", test_tags},       {"decoders", test_decoders}, {"queue", test_queue},   {"browse", test_browse},
        {"resume", test_resume},   {"media_keys", test_media_keys}, {"engine", test_engine}, {"outputs", test_outputs},
        {"launcher", test_launcher}, {"art", test_art},         {"layout", test_layout}, {"real_libs", test_real_libs},
        {"transport", test_transport}, {"hits", test_hits}, {"volume", test_volume},
    };
    for (const Group &g : groups) {
        if (only && std::strcmp(only, g.name) != 0) continue;
        const int f0 = g_fail, p0 = g_pass;
        g.fn();
        std::fprintf(stderr, "== %-10s %d passed, %d failed\n", g.name, g_pass - p0, g_fail - f0);
    }
    std::printf("vinyl tests: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
