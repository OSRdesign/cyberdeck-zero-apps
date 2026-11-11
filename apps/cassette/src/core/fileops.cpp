/*
 * SPDX-License-Identifier: MIT
 */
#include "fileops.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

namespace cassette {
namespace {

template <typename T> T neg(T r) { return r < 0 ? static_cast<T>(-errno) : r; }

class PosixFileOps final : public FileOps {
public:
    int open(const std::string &p, int flags, int mode) override { return neg(::open(p.c_str(), flags | O_CLOEXEC, mode)); }
    int close(int fd) override { return neg(::close(fd)); }
    ssize_t write(int fd, const void *b, size_t n) override { return neg(::write(fd, b, n)); }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t off) override {
        return neg(::pwrite(fd, b, n, static_cast<off_t>(off)));
    }
    ssize_t pread(int fd, void *b, size_t n, uint64_t off) override {
        return neg(::pread(fd, b, n, static_cast<off_t>(off)));
    }
    int fdatasync(int fd) override { return neg(::fdatasync(fd)); }
    int fsync(int fd) override { return neg(::fsync(fd)); }
    int ftruncate(int fd, uint64_t len) override { return neg(::ftruncate(fd, static_cast<off_t>(len))); }
    int fstat(int fd, StatInfo &st) override {
        struct stat s;
        if (::fstat(fd, &s) < 0) return -errno;
        st.size = static_cast<uint64_t>(s.st_size);
        st.mtime = static_cast<int64_t>(s.st_mtime);
        return 0;
    }
    int stat(const std::string &p, StatInfo &st) override {
        struct stat s;
        if (::stat(p.c_str(), &s) < 0) return -errno;
        st.size = static_cast<uint64_t>(s.st_size);
        st.mtime = static_cast<int64_t>(s.st_mtime);
        return 0;
    }
    int rename(const std::string &a, const std::string &b) override { return neg(::rename(a.c_str(), b.c_str())); }
    int unlink(const std::string &p) override { return neg(::unlink(p.c_str())); }
    int mkdir(const std::string &p) override {
        if (::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST) return 0;
        return -errno;
    }
    int fsync_dir(const std::string &p) override {
        int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return -errno;
        int r = ::fsync(fd);
        int e = errno;
        ::close(fd);
        return r < 0 ? -e : 0;
    }
    int list_dir(const std::string &p, std::vector<std::string> &names) override {
        DIR *d = ::opendir(p.c_str());
        if (!d) return -errno;
        while (const dirent *e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") names.push_back(n);
        }
        ::closedir(d);
        return 0;
    }
    int free_space(const std::string &p, uint64_t &avail) override {
        struct statvfs v;
        if (::statvfs(p.c_str(), &v) < 0) return -errno;
        avail = static_cast<uint64_t>(v.f_bavail) * static_cast<uint64_t>(v.f_frsize);
        return 0;
    }
};

class SystemClock final : public Clock {
public:
    uint64_t mono_ms() override {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
    int64_t wall_s() override { return static_cast<int64_t>(::time(nullptr)); }
    void sleep_ms(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

}  // namespace

FileOps &posix_file_ops() {
    static PosixFileOps o;
    return o;
}
Clock &system_clock() {
    static SystemClock c;
    return c;
}

int write_all(FileOps &ops, int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    int stalls = 0;
    while (n > 0) {
        ssize_t w = ops.write(fd, p, n);
        if (w < 0) {
            if ((w == -EINTR || w == -EAGAIN) && ++stalls < 100) continue;
            return static_cast<int>(w);
        }
        if (w == 0) {
            if (++stalls < 100) continue;
            return -EIO;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return 0;
}

}  // namespace cassette
