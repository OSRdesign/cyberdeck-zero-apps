/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette core: the one seam for every file-system call and the clock. The real implementation is POSIX; the tests
 * wrap it to inject failures (ENOSPC, EIO, short writes) and use a fake clock. LVGL-free.
 * Every call returns >= 0 on success (bytes for the read and write calls) and -errno on failure.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace vinyl {

struct StatInfo {
    uint64_t size = 0;
    int64_t mtime = 0; /* seconds since the epoch */
};

class FileOps {
public:
    virtual ~FileOps() = default;
    virtual int open(const std::string &path, int flags, int mode) = 0; /* a file descriptor */
    virtual int close(int fd) = 0;
    virtual ssize_t write(int fd, const void *buf, size_t n) = 0;
    virtual ssize_t pwrite(int fd, const void *buf, size_t n, uint64_t off) = 0;
    virtual ssize_t pread(int fd, void *buf, size_t n, uint64_t off) = 0;
    virtual int fdatasync(int fd) = 0;
    virtual int fsync(int fd) = 0;
    virtual int ftruncate(int fd, uint64_t len) = 0;
    virtual int fstat(int fd, StatInfo &st) = 0;
    virtual int stat(const std::string &path, StatInfo &st) = 0;
    virtual int rename(const std::string &from, const std::string &to) = 0;
    virtual int unlink(const std::string &path) = 0;
    virtual int mkdir(const std::string &path) = 0; /* 0 also when it exists */
    virtual int fsync_dir(const std::string &path) = 0;
    virtual int list_dir(const std::string &path, std::vector<std::string> &names) = 0; /* names only */
    virtual int free_space(const std::string &path, uint64_t &avail_bytes) = 0;        /* the statvfs seam */
};

class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t mono_ms() = 0; /* monotonic */
    virtual int64_t wall_s() = 0;   /* seconds since the epoch */
    virtual void sleep_ms(uint32_t ms) = 0;
};

FileOps &posix_file_ops();
Clock &system_clock();

/* Writes the whole buffer (loops over short writes and EINTR): 0 or -errno. */
int write_all(FileOps &ops, int fd, const void *buf, size_t n);

}  // namespace vinyl
