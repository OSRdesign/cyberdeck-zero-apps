/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the output volume's small state file (see volume_state.hpp). The three accessors are Vinyl's (state/resume.cpp).
 */
#include "volume_state.hpp"

#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace vhs {

int app_volume_of(const VolumeState &s, const std::string &key) {
    auto it = s.app_volumes.find(key);
    return it != s.app_volumes.end() ? it->second : 100;
}

void set_app_volume(VolumeState &s, const std::string &key, int level) {
    level = std::max(0, std::min(100, level));
    if (key.empty()) return;
    if (!s.app_volumes.count(key) && s.app_volumes.size() >= kMaxAppVolumes) s.app_volumes.erase(s.app_volumes.begin());
    s.app_volumes[key] = level;
}

bool app_muted_of(const VolumeState &s, const std::string &key) { return !key.empty() && s.app_muted.count(key) != 0; }

void set_app_muted(VolumeState &s, const std::string &key, bool muted) {
    if (key.empty()) return;
    if (!muted) {
        s.app_muted.erase(key);
        return;
    }
    if (!s.app_muted.count(key) && s.app_muted.size() >= kMaxAppVolumes) s.app_muted.erase(s.app_muted.begin());
    s.app_muted.insert(key);
}

std::string volume_state_path() {
    const char *ov = std::getenv("VHS_OUTPUT_LEVELS");
    if (ov && *ov) return ov;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/vhs/output-levels";
    const char *home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/vhs/output-levels";
    return "";
}

std::string volume_state_to_text(const VolumeState &s) {
    std::string out = "v1\n";
    std::set<std::string> keys;
    for (const auto &kv : s.app_volumes) keys.insert(kv.first);
    for (const std::string &k : s.app_muted) keys.insert(k);
    for (const std::string &k : keys) {
        out += std::to_string(app_volume_of(s, k)) + "\t" + (app_muted_of(s, k) ? "m" : "u") + "\t" + k + "\n";
    }
    return out;
}

bool volume_state_from_text(const std::string &text, VolumeState &out) {
    out = VolumeState();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "v1") return false;
    while (std::getline(in, line)) {
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        char *end = nullptr;
        const std::string pct = line.substr(0, a);
        const long v = std::strtol(pct.c_str(), &end, 10);
        if (!end || *end != 0 || pct.empty()) continue;
        const std::string flag = line.substr(a + 1, b - a - 1);
        const std::string key = line.substr(b + 1);
        if (key.empty() || (flag != "m" && flag != "u")) continue;
        set_app_volume(out, key, static_cast<int>(std::max(0L, std::min(100L, v))));
        if (flag == "m") set_app_muted(out, key, true);
    }
    return true;
}

int save_volume_state(const std::string &path, const VolumeState &s) {
    if (path.empty()) return -ENOENT;
    FileOps &ops = posix_file_ops();
    const int r = make_dirs(ops, dir_name(path));
    if (r < 0) return r;
    return atomic_write_file(ops, path, volume_state_to_text(s));
}

bool load_volume_state(const std::string &path, VolumeState &out) {
    out = VolumeState();
    if (path.empty()) return false;
    std::string text;
    if (read_small_file(posix_file_ops(), path, text, 64 * 1024) < 0) return false;
    return volume_state_from_text(text, out);
}

}  // namespace vhs
