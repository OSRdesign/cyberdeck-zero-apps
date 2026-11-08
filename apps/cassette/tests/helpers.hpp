/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core tests: a temp folder, a fake clock, a fault-injecting FileOps, and the synthetic test signal.
 * All audio in the tests is generated here (a sawtooth tone with noise); no recording is ever used.
 */
#pragma once

#include "fileops.hpp"
#include "wav.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace tst {

using namespace cassette;

struct TempDir {
    std::string path;
    TempDir() {
        std::string t = (std::filesystem::temp_directory_path() / "cassette-test-XXXXXX").string();
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

/* Mono 16-bit sample number i of the test signal: a sawtooth tone plus a little noise, deterministic. */
inline int16_t sig(uint64_t i) {
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    int noise = static_cast<int>(x & 0xFF) - 128;
    int saw = (static_cast<int>(i % 200) - 100) * 250;
    return static_cast<int16_t>(saw + noise);
}

inline std::vector<int16_t> sig_block(uint64_t start, size_t n) {
    std::vector<int16_t> v(n);
    for (size_t k = 0; k < n; k++) v[k] = sig(start + k);
    return v;
}

inline std::vector<unsigned char> read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline void write_file(const std::string &p, const std::vector<unsigned char> &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

/* Do the audio bytes of the file at p (after data_offset) equal sig(first .. first+frames-1) as mono S16? */
inline bool audio_is_sig(const std::string &p, uint64_t data_offset, uint64_t first, uint64_t frames) {
    std::vector<unsigned char> b = read_file(p);
    if (b.size() < data_offset + frames * 2) return false;
    for (uint64_t k = 0; k < frames; k++) {
        const size_t o = static_cast<size_t>(data_offset + k * 2);
        int16_t v = static_cast<int16_t>(b[o] | (b[o + 1] << 8));
        if (v != sig(first + k)) return false;
    }
    return true;
}

class FakeClock final : public Clock {
public:
    uint64_t now = 1000;
    int64_t wall = 1700000000;
    uint64_t mono_ms() override { return now; }
    int64_t wall_s() override { return wall; }
    void sleep_ms(uint32_t ms) override { advance(ms); }
    void advance(uint64_t ms) {
        now += ms;
        wall += static_cast<int64_t>(ms / 1000);
    }
};

/* The real POSIX calls with switches for failures, a call log, and faked stat results. */
class FaultOps final : public FileOps {
public:
    struct Ev {
        std::string op;
        uint64_t a = 0, b = 0, at = 0;
    };
    explicit FaultOps(Clock *c = nullptr) : clk(c), base(posix_file_ops()) {}

    Clock *clk;
    FileOps &base;
    std::vector<Ev> log;
    int64_t write_budget = -1;   /* bytes write() may still take; the call that crosses it writes the rest, then fails */
    int write_errno = 5;         /* EIO by default */
    size_t short_max = 0;        /* write() takes at most this many bytes per call */
    int fdatasync_errno = 0;
    int fdatasync_fail_from = -1; /* fail the n-th fdatasync (0-based) and the later ones */
    int fdatasync_calls = 0;
    int rename_errno = 0;
    std::function<uint64_t()> free_fn;
    int64_t stat_size = -1;      /* stat() reports this size when >= 0 */
    int64_t stat_mtime = -1;

    void rec(const char *op, uint64_t a = 0, uint64_t b = 0) { log.push_back(Ev{op, a, b, clk ? clk->mono_ms() : 0}); }
    int count(const std::string &op) const {
        int n = 0;
        for (const Ev &e : log) n += e.op == op;
        return n;
    }

    std::set<int> wav_fds; /* write faults apply to the take files only, not to the journal's small files */
    int open(const std::string &p, int f, int m) override {
        rec("open");
        int fd = base.open(p, f, m);
        if (fd >= 0 && p.size() > 4 && p.compare(p.size() - 4, 4, ".wav") == 0) wav_fds.insert(fd);
        return fd;
    }
    int close(int fd) override {
        rec("close");
        wav_fds.erase(fd);
        return base.close(fd);
    }
    ssize_t write(int fd, const void *b, size_t n) override {
        if (!wav_fds.count(fd)) return base.write(fd, b, n);
        if (write_budget == 0) return -write_errno;
        if (short_max && n > short_max) n = short_max;
        if (write_budget > 0 && static_cast<int64_t>(n) > write_budget) n = static_cast<size_t>(write_budget);
        ssize_t w = base.write(fd, b, n);
        if (w > 0 && write_budget > 0) write_budget -= w;
        rec("write", static_cast<uint64_t>(w < 0 ? 0 : w));
        return w;
    }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t off) override {
        rec("pwrite", off, n);
        return base.pwrite(fd, b, n, off);
    }
    ssize_t pread(int fd, void *b, size_t n, uint64_t off) override { return base.pread(fd, b, n, off); }
    int fdatasync(int fd) override {
        rec("fdatasync");
        int k = fdatasync_calls++;
        if (fdatasync_errno && (fdatasync_fail_from < 0 || k >= fdatasync_fail_from)) return -fdatasync_errno;
        return base.fdatasync(fd);
    }
    int fsync(int fd) override { rec("fsync"); return base.fsync(fd); }
    int ftruncate(int fd, uint64_t len) override { rec("ftruncate", len); return base.ftruncate(fd, len); }
    int fstat(int fd, StatInfo &st) override { return base.fstat(fd, st); }
    int stat(const std::string &p, StatInfo &st) override {
        int r = base.stat(p, st);
        if (r == 0 && stat_size >= 0) st.size = static_cast<uint64_t>(stat_size);
        if (r == 0 && stat_mtime >= 0) st.mtime = stat_mtime;
        return r;
    }
    int rename(const std::string &a, const std::string &b) override {
        rec("rename");
        if (rename_errno) return -rename_errno;
        return base.rename(a, b);
    }
    int unlink(const std::string &p) override { return base.unlink(p); }
    int mkdir(const std::string &p) override { return base.mkdir(p); }
    int fsync_dir(const std::string &p) override { rec("fsync_dir"); return base.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return base.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &avail) override {
        if (free_fn) {
            avail = free_fn();
            return 0;
        }
        return base.free_space(p, avail);
    }
};

}  // namespace tst
