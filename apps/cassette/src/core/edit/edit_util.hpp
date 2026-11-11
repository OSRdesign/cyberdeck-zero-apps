/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core, edit unit: small shared helpers (little-endian access, temporary names, whole-buffer reads). LVGL-free.
 */
#pragma once

#include "fileops.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unistd.h>

namespace cassette {
namespace editutil {

constexpr const char *kTmpPrefix = ".tmp-";

inline void put16(unsigned char *p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
}
inline void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline void put64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
inline uint32_t get16(const unsigned char *p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
inline uint32_t get32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline uint64_t get64(const unsigned char *p) { return get32(p) | static_cast<uint64_t>(get32(p + 4)) << 32; }

/* "<dir>/.tmp-<tag><random hex>": unique per call and per process. */
inline std::string tmp_name(const std::string &dir, const char *tag = "") {
    static std::atomic<unsigned> counter{0};
    static std::random_device rd;
    char buf[80];
    const unsigned a = rd(), b = rd();
    std::snprintf(buf, sizeof buf, "%s%s%08x%08x%04x", kTmpPrefix, tag, a, b,
                  (static_cast<unsigned>(getpid()) ^ (counter++ << 8)) & 0xFFFFu);
    return dir + "/" + buf;
}

inline std::string dir_of(const std::string &path) {
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string(".") : s == 0 ? std::string("/") : path.substr(0, s);
}
inline std::string base_of(const std::string &path) {
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

/* Reads up to n bytes at off; returns the bytes read (less only at end of file) or -errno. */
inline int64_t pread_full(FileOps &ops, int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    int stalls = 0;
    while (got < n) {
        ssize_t k = ops.pread(fd, static_cast<char *>(buf) + got, n - got, off + got);
        if (k < 0) {
            if (k == -EINTR && ++stalls < 100) continue;
            return k;
        }
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    return static_cast<int64_t>(got);
}

inline int pwrite_all(FileOps &ops, int fd, const void *buf, size_t n, uint64_t off) {
    const char *p = static_cast<const char *>(buf);
    int stalls = 0;
    while (n > 0) {
        ssize_t w = ops.pwrite(fd, p, n, off);
        if (w < 0) {
            if (w == -EINTR && ++stalls < 100) continue;
            return static_cast<int>(w);
        }
        if (w == 0) {
            if (++stalls < 100) continue;
            return -EIO;
        }
        p += w;
        off += static_cast<uint64_t>(w);
        n -= static_cast<size_t>(w);
    }
    return 0;
}

/* Closes the fd on scope exit (does nothing else to the file system: a kill-style unwinding stays a kill). */
struct FdGuard {
    FileOps &ops;
    int fd;
    FdGuard(FileOps &o, int f) : ops(o), fd(f) {}
    ~FdGuard() {
        if (fd >= 0) ops.close(fd);
    }
    int release() {
        int f = fd;
        fd = -1;
        return f;
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
};

}  // namespace editutil
}  // namespace cassette
