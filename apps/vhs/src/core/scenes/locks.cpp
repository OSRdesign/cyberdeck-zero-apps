/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the locked files (see locks.hpp).
 */
#include "locks.hpp"

#include "atomic_file.hpp"
#include "fileops.hpp"

#include <sstream>

namespace vhs {

std::set<std::string> read_locks(FileOps &ops, const std::string &folder)
{
    std::set<std::string> out;
    std::string text;
    if (read_small_file(ops, folder + "/.locked", text, 1u << 20) != 0) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line.find('/') == std::string::npos) out.insert(line);
    }
    return out;
}

int write_locks(FileOps &ops, const std::string &folder, const std::set<std::string> &names)
{
    const std::string path = folder + "/.locked";
    if (names.empty()) {
        const int r = ops.unlink(path);
        return r == -ENOENT ? 0 : r;
    }
    std::string text;
    for (const std::string &n : names) text += n + "\n";
    return atomic_write_file(ops, path, text);
}

int set_locked(FileOps &ops, const std::string &folder, const std::string &name, bool locked)
{
    std::set<std::string> s = read_locks(ops, folder);
    const bool had = s.count(name) != 0;
    if (had == locked) return 0;
    if (locked) s.insert(name);
    else s.erase(name);
    return write_locks(ops, folder, s);
}

} // namespace vhs
