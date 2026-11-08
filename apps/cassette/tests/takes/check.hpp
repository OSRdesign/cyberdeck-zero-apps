/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes tests: the CHECK macros (the pattern of tests/check.hpp), a temp folder, a fault-injecting FileOps and
 * synthetic WAV files. All files in these tests are generated here; no recording is ever used.
 */
#pragma once

#include "fileops.hpp"
#include "wav.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

extern int g_pass, g_fail;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        const auto va_ = (a);                                                                                          \
        const auto vb_ = (b);                                                                                          \
        if (va_ == vb_) ++g_pass;                                                                                      \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed: [%s] vs [%s]\n", __FILE__, __LINE__, #a, #b,          \
                         test_text(va_).c_str(), test_text(vb_).c_str());                                              \
        }                                                                                                              \
    } while (0)

inline std::string test_text(const std::string &s) { return s; }
inline std::string test_text(const char *s) { return std::string(s ? s : ""); }
template <typename T> std::string test_text(const T &v) { return std::to_string(v); }

void test_json();
void test_naming();
void test_counter();
void test_edits();
void test_recordings();
void test_settings();

namespace tk {

using namespace cassette;

struct TempDir {
    std::string path;
    TempDir() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-takes-XXXXXX").string();
        std::vector<char> b(t.begin(), t.end());
        b.push_back('\0');
        if (!mkdtemp(b.data())) std::abort();
        path = b.data();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    std::string file(const std::string &n) const { return path + "/" + n; }
};

inline bool exists(const std::string &p) { return std::filesystem::exists(p); }

inline std::string read_text(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline void write_bytes(const std::string &p, const void *b, size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char *>(b), static_cast<std::streamsize>(n));
}
inline void write_text(const std::string &p, const std::string &s) { write_bytes(p, s.data(), s.size()); }

/* A complete synthetic WAV: Cassette's header and `frames` frames of a deterministic ramp. */
inline void make_wav(const std::string &p, uint64_t frames, const AudioFormat &fmt = AudioFormat(),
                     const BwfInfo &info = BwfInfo()) {
    const uint32_t ba = block_align(fmt);
    std::vector<unsigned char> b = build_header(fmt, info, frames * ba);
    for (uint64_t k = 0; k < frames * ba; k++) b.push_back(static_cast<unsigned char>((k * 37u) & 0xFF));
    write_bytes(p, b.data(), b.size());
}

/* The real POSIX calls with switches for failures. */
class FailOps final : public FileOps {
public:
    FileOps &base = posix_file_ops();
    int rename_errno = 0;   /* every rename fails with this */
    int fsync_errno = 0;    /* every fsync fails with this */
    int list_errno = 0;
    std::function<uint64_t()> free_fn;
    int renames = 0, unlinks = 0;

    int open(const std::string &p, int f, int m) override { return base.open(p, f, m); }
    int close(int fd) override { return base.close(fd); }
    ssize_t write(int fd, const void *b, size_t n) override { return base.write(fd, b, n); }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t off) override { return base.pwrite(fd, b, n, off); }
    ssize_t pread(int fd, void *b, size_t n, uint64_t off) override { return base.pread(fd, b, n, off); }
    int fdatasync(int fd) override { return base.fdatasync(fd); }
    int fsync(int fd) override { return fsync_errno ? -fsync_errno : base.fsync(fd); }
    int ftruncate(int fd, uint64_t len) override { return base.ftruncate(fd, len); }
    int fstat(int fd, StatInfo &st) override { return base.fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override { return base.stat(p, st); }
    int rename(const std::string &a, const std::string &b) override {
        renames++;
        return rename_errno ? -rename_errno : base.rename(a, b);
    }
    int unlink(const std::string &p) override {
        unlinks++;
        return base.unlink(p);
    }
    int mkdir(const std::string &p) override { return base.mkdir(p); }
    int fsync_dir(const std::string &p) override { return base.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override {
        return list_errno ? -list_errno : base.list_dir(p, n);
    }
    int free_space(const std::string &p, uint64_t &avail) override {
        if (free_fn) {
            avail = free_fn();
            return 0;
        }
        return base.free_space(p, avail);
    }
};

}  // namespace tk
