/*
 * SPDX-License-Identifier: MIT
 */
#include "fsutil.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mdnotes {

namespace {

void set_err(int *err, int value)
{
    if (err) *err = value;
}

bool write_all(int fd, const std::string &data, int *err)
{
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(err, errno);
            return false;
        }
        if (n == 0) {
            set_err(err, EIO);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

FileStat stat_path(const std::string &path, bool follow)
{
    FileStat fs;
    struct stat st;
    const int rc = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (rc != 0) return fs;
    fs.exists = true;
    fs.is_dir = S_ISDIR(st.st_mode);
    fs.is_reg = S_ISREG(st.st_mode);
    fs.is_link = S_ISLNK(st.st_mode);
    fs.size = static_cast<std::int64_t>(st.st_size);
    fs.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    fs.mode = static_cast<unsigned>(st.st_mode & 07777);
    return fs;
}

ReadStatus read_file(const std::string &path, std::string &out, std::size_t max_bytes, int *err)
{
    out.clear();
    set_err(err, 0);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, errno);
        return (errno == ENOENT || errno == ENOTDIR) ? ReadStatus::Missing : ReadStatus::Error;
    }
    struct stat st;
    if (::fstat(fd, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            ::close(fd);
            set_err(err, EISDIR);
            return ReadStatus::Error;
        }
        if (st.st_size > 0 && static_cast<std::uint64_t>(st.st_size) > max_bytes) {
            ::close(fd);
            return ReadStatus::TooBig;
        }
        if (st.st_size > 0) out.reserve(static_cast<std::size_t>(st.st_size));
    }
    char buf[16384];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(err, errno);
            ::close(fd);
            out.clear();
            return ReadStatus::Error;
        }
        if (n == 0) break;
        if (out.size() + static_cast<std::size_t>(n) > max_bytes) {   /* grew while we read it */
            ::close(fd);
            out.clear();
            return ReadStatus::TooBig;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return ReadStatus::Ok;
}

std::string temp_path_for(const std::string &path)
{
    return path_join(dir_name(path), "." + base_name(path) + ".~deck");
}

bool fsync_dir(const std::string &dir)
{
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool write_file_atomic(const std::string &path, const std::string &data, int *err)
{
    set_err(err, 0);
    const std::string tmp = temp_path_for(path);
    unsigned mode = 0644;
    const FileStat old = stat_path(path);
    if (old.exists && old.is_reg) mode = old.mode & 07777;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_err(err, errno);
        return false;
    }
    bool ok = write_all(fd, data, err);
    if (ok && ::fchmod(fd, static_cast<mode_t>(mode)) != 0) {
        set_err(err, errno);
        ok = false;
    }
    if (ok && ::fsync(fd) != 0) {
        set_err(err, errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        set_err(err, errno);
        ok = false;
    }
    if (ok && ::rename(tmp.c_str(), path.c_str()) != 0) {
        set_err(err, errno);
        ok = false;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    fsync_dir(dir_name(path));   /* best effort: the rename is done; some file systems refuse a folder fsync */
    return true;
}

bool create_file_exclusive(const std::string &path, const std::string &data, int *err)
{
    set_err(err, 0);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_err(err, errno);
        return false;
    }
    bool ok = write_all(fd, data, err);
    if (ok && ::fsync(fd) != 0) {
        set_err(err, errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        set_err(err, errno);
        ok = false;
    }
    if (!ok) {
        ::unlink(path.c_str());
        return false;
    }
    fsync_dir(dir_name(path));
    return true;
}

bool make_dirs(const std::string &path, int *err)
{
    set_err(err, 0);
    if (path.empty()) {
        set_err(err, ENOENT);
        return false;
    }
    const FileStat top = stat_path(path);
    if (top.exists) {
        if (top.is_dir) return true;
        set_err(err, ENOTDIR);
        return false;
    }
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string cur = path.substr(0, j);
        if (!cur.empty()) {
            const FileStat fs = stat_path(cur);
            if (!fs.exists) {
                if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                    set_err(err, errno);
                    return false;
                }
            } else if (!fs.is_dir) {
                set_err(err, ENOTDIR);
                return false;
            }
        }
        i = j + 1;
    }
    return stat_path(path).is_dir;
}

bool list_dir(const std::string &path, std::vector<DirEntry> &out, int *err)
{
    out.clear();
    set_err(err, 0);
    DIR *d = ::opendir(path.c_str());
    if (!d) {
        set_err(err, errno);
        return false;
    }
    while (struct dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        DirEntry de;
        de.name = n;
        const FileStat fs = stat_path(path_join(path, n), false);
        de.is_link = fs.is_link;
        de.is_dir = fs.is_dir;
        de.is_reg = fs.is_reg;
        de.size = fs.size;
        de.mtime_ns = fs.mtime_ns;
        out.push_back(std::move(de));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    return true;
}

bool remove_tree(const std::string &path)
{
    const FileStat fs = stat_path(path, false);
    if (!fs.exists) return true;
    if (!fs.is_dir) return ::unlink(path.c_str()) == 0;
    ::chmod(path.c_str(), 0755);   /* a test may have made it read-only */
    std::vector<DirEntry> entries;
    list_dir(path, entries);
    bool ok = true;
    for (const DirEntry &e : entries) ok = remove_tree(path_join(path, e.name)) && ok;
    return (::rmdir(path.c_str()) == 0) && ok;
}

std::string path_join(const std::string &a, const std::string &b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string dir_name(const std::string &path)
{
    const std::size_t p = path.rfind('/');
    if (p == std::string::npos) return std::string();
    if (p == 0) return "/";
    return path.substr(0, p);
}

std::string base_name(const std::string &path)
{
    const std::size_t p = path.rfind('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string home_dir()
{
    if (const char *h = std::getenv("HOME"); h && *h) return h;
    if (const struct passwd *pw = ::getpwuid(::getuid()); pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    return "/";
}

std::string current_dir()
{
    char buf[4096];
    if (::getcwd(buf, sizeof buf)) return buf;
    return std::string();
}

} // namespace mdnotes
