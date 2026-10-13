/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: small file helpers (see fsutil.hpp).
 */
#include "fsutil.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace sc {
namespace fs {

bool exists(const std::string &path)
{
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string read_all(const std::string &path, bool *ok)
{
    if (ok) *ok = false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string();
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > (1u << 20)) break;
    }
    close(fd);
    if (ok) *ok = true;
    return text;
}

std::string read_line(const std::string &path)
{
    std::string text = read_all(path);
    const std::size_t end = text.find_first_of("\r\n");
    if (end != std::string::npos) text.resize(end);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    return text;
}

std::string read_link(const std::string &path)
{
    char buf[PATH_MAX];
    const ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string real_path(const std::string &path)
{
    char *resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return std::string();
    std::string out(resolved);
    std::free(resolved);
    return out;
}

std::string base_name(const std::string &path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const std::size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string parent(const std::string &path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const std::size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

std::vector<std::string> list_dir(const std::string &path)
{
    std::vector<std::string> names;
    DIR *dir = opendir(path.c_str());
    if (!dir) return names;
    while (const struct dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

bool make_dirs(const std::string &path, unsigned mode)
{
    if (path.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), static_cast<mode_t>(mode)) != 0 && errno != EEXIST) return false;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_file(const std::string &path, const std::string &text)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return close(fd) == 0;
}

bool write_atomic(const std::string &path, const std::string &text)
{
    const std::string tmp = path + ".tmp" + std::to_string(getpid());
    if (!write_file(tmp, text)) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool make_link(const std::string &target, const std::string &path)
{
    unlink(path.c_str());
    return symlink(target.c_str(), path.c_str()) == 0;
}

bool remove_tree(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        chmod(path.c_str(), 0755);
        for (const std::string &name : list_dir(path)) remove_tree(path + "/" + name);
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

} // namespace fs
} // namespace sc
