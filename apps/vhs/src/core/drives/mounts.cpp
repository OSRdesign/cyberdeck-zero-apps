/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at origin/main fdad829) for VHS (brief 3.1, 9.3), from library/roots.cpp (see
 * mounts.hpp).
 */
#include "mounts.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace vhs {

namespace {

std::string unescape(const std::string &s) { /* /proc/mounts writes a space as \040 */
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
            o += static_cast<char>(std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

}  // namespace

std::vector<MountEntry> parse_mounts(const std::string &text) {
    std::vector<MountEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        MountEntry m;
        if (!(ls >> m.dev >> m.dir >> m.type)) continue;
        m.dev = unescape(m.dev);
        m.dir = unescape(m.dir);
        out.push_back(m);
    }
    return out;
}

std::string read_mounts() {
    std::ifstream f("/proc/self/mounts");
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace vhs
