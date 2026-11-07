/*
 * SPDX-License-Identifier: MIT
 */

#include "port_busy.hpp"

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <limits.h>
#include <unistd.h>

namespace meshzero {

namespace {

std::string resolved(const std::string &p)
{
    char real[PATH_MAX];
    return ::realpath(p.c_str(), real) ? std::string(real) : p;
}

bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9') return false;
    return true;
}

std::string comm_of(const std::string &proc_dir, const std::string &pid)
{
    std::ifstream in(proc_dir + "/" + pid + "/comm");
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

} // namespace

bool port_in_use(const std::string &path, PortUser *who, const std::string &proc_dir, int self_pid)
{
    if (path.empty()) return false;
    if (self_pid < 0) self_pid = static_cast<int>(::getpid());
    const std::string target = resolved(path);
    DIR *procs = ::opendir(proc_dir.c_str());
    if (!procs) return false;
    bool found = false;
    while (!found) {
        const dirent *pe = ::readdir(procs);
        if (!pe) break;
        if (!all_digits(pe->d_name) || std::atoi(pe->d_name) == self_pid) continue;
        const std::string fd_dir = proc_dir + "/" + pe->d_name + "/fd";
        DIR *fds = ::opendir(fd_dir.c_str());
        if (!fds) continue;                       // not ours to read (or gone): unknown, not busy
        while (const dirent *fe = ::readdir(fds)) {
            if (fe->d_name[0] == '.') continue;
            char link[PATH_MAX];
            const ssize_t n = ::readlink((fd_dir + "/" + fe->d_name).c_str(), link, sizeof(link) - 1);
            if (n <= 0) continue;
            link[n] = 0;
            if (link[0] != '/') continue;         // pipe:[123], socket:[456], anon_inode:...
            if (link == target || link == path || resolved(link) == target) {
                found = true;
                break;
            }
        }
        ::closedir(fds);
        if (found && who) {
            who->pid = std::atoi(pe->d_name);
            who->name = comm_of(proc_dir, pe->d_name);
        }
    }
    ::closedir(procs);
    return found;
}

std::string port_busy_text(const PortUser &who)
{
    return "This port is used by " + (who.name.empty() ? std::string("another program") : who.name) + ". Close that program first.";
}

} // namespace meshzero
