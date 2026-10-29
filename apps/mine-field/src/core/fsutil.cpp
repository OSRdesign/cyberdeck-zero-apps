/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: file operations (see fsutil.hpp).
 */
#include "fsutil.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace minefield {

bool read_file(const std::string &path, std::string &out, std::size_t max_bytes)
{
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::size_t>(st.st_size) > max_bytes) {
        ::close(fd);
        return false;
    }
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            out.clear();
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > max_bytes) {
            ::close(fd);
            out.clear();
            return false;
        }
    }
    ::close(fd);
    return true;
}

static std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool write_file_atomic(const std::string &path, const std::string &data)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    const int dfd = ::open(parent_of(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        cur = path.substr(0, next);
        if (!cur.empty() && ::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
        pos = next + 1;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool path_exists(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool rename_path(const std::string &from, const std::string &to)
{
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool remove_file(const std::string &path)
{
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

} // namespace minefield
