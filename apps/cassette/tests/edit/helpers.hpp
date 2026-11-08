/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: an in-memory FileOps with kill and failure injection at every step, synthetic sources, a fake name
 * provider and the crash invariants. All audio is generated here; no recording is ever used.
 */
#pragma once

#include "fileops.hpp"
#include "trim_split.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace tst {

using namespace cassette;
using Bytes = std::vector<unsigned char>;

/* ---------------------------------------------------------------- generated audio */

inline uint64_t mix(uint64_t x) {
    x = (x + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return x;
}

/* A smooth-ish 16-bit test signal: sawtooth plus noise (deterministic). */
inline int sig(uint64_t i) {
    int noise = static_cast<int>(mix(i) & 0xFF) - 128;
    int saw = (static_cast<int>(i % 200) - 100) * 250;
    return saw + noise;
}

/* The sample of frame i, channel c in the NATIVE width of the format. The 16-bit scale value is sig(i + 977*c); the lower
 * bits are filled so that (native >> (bits-16)) == sig(...). */
inline int32_t sample_native(uint64_t i, unsigned c, unsigned bits) {
    const int32_t s = sig(i + 977ull * c);
    if (bits == 16) return s;
    const int32_t low = static_cast<int32_t>(mix(i * 7 + c) & 0xFF);
    if (bits == 24) return s * 256 + low;
    return s * 65536 + low * 257;
}
/* The same sample on the 16-bit scale. */
inline int sample16(uint64_t i, unsigned c) { return sig(i + 977ull * c); }

inline Bytes make_audio(const AudioFormat &f, uint64_t first, uint64_t frames) {
    const unsigned sb = f.bits / 8u;
    Bytes b(static_cast<size_t>(frames) * f.channels * sb);
    size_t o = 0;
    for (uint64_t k = 0; k < frames; k++)
        for (unsigned c = 0; c < f.channels; c++) {
            const int32_t v = sample_native(first + k, c, f.bits);
            for (unsigned j = 0; j < sb; j++) b[o++] = static_cast<unsigned char>(static_cast<uint32_t>(v) >> (8 * j));
        }
    return b;
}

struct SrcSpec {
    AudioFormat fmt;
    uint64_t frames = 12000;
    uint64_t tref = 123456;
    std::string desc = "Interview";
    std::string history; /* empty: the default line */
    enum Kind { Finalised, Placeholder, Arecord44 } kind = Finalised;
};

inline void put32le(Bytes &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
}

inline Bytes make_source(const SrcSpec &s) {
    Bytes audio = make_audio(s.fmt, 0, s.frames);
    Bytes file;
    if (s.kind == SrcSpec::Arecord44) {
        /* the plain arecord header: 44 bytes, no bext, sizes left as placeholders */
        file.assign(44, 0);
        std::memcpy(&file[0], "RIFF", 4);
        put32le(file, 4, 0x80000024u);
        std::memcpy(&file[8], "WAVEfmt ", 8);
        put32le(file, 16, 16);
        file[20] = 1;
        file[22] = static_cast<unsigned char>(s.fmt.channels);
        put32le(file, 24, s.fmt.rate);
        put32le(file, 28, s.fmt.rate * block_align(s.fmt));
        file[32] = static_cast<unsigned char>(block_align(s.fmt));
        file[34] = static_cast<unsigned char>(s.fmt.bits);
        std::memcpy(&file[36], "data", 4);
        put32le(file, 40, 0x80000000u);
    } else {
        BwfInfo info;
        info.description = s.desc;
        info.originator_reference = "REF1";
        info.origination_date = "2026-11-08";
        info.origination_time = "11:05:00";
        info.time_reference = s.tref;
        info.coding_history = s.history;
        file = build_header(s.fmt, info, s.kind == SrcSpec::Placeholder ? 0 : audio.size());
        if (s.kind == SrcSpec::Placeholder) {
            put32le(file, 4, 0x80000024u);
            put32le(file, kDataOffset - 4, 0x80000000u);
        }
    }
    file.insert(file.end(), audio.begin(), audio.end());
    return file;
}

/* ---------------------------------------------------------------- in-memory file system */

class MemOps final : public FileOps {
public:
    struct File {
        Bytes data;                 /* the stored bytes (all of them, or the first kCap of a discarding file) */
        uint64_t size = 0;          /* the logical size */
        bool discard = false;       /* keep only the first kCap bytes (mocked huge outputs) */
        std::function<void(uint64_t, unsigned char *, size_t)> gen; /* a generated (read-only) file */
        int64_t mtime = 0;
    };
    static constexpr uint64_t kCap = 65536;

    std::map<std::string, std::shared_ptr<File>> files;
    std::vector<std::string> log; /* every call, with its path */
    int step = 0;                 /* counted calls (all but close, stat, fstat, list, free space) */
    int kill_at = -1;             /* the call with this number does not happen, nor does anything after (SIGKILL) */
    int fail_at = -1;             /* the call with this number fails once with fail_errno */
    int fail_errno = EIO;
    bool dead = false;
    bool tear_writes = true;      /* a killed write() leaves half of its bytes */
    uint64_t free_bytes = 1ull << 40;
    int64_t clock = 1000;
    size_t max_pread = 0;         /* the largest pread seen */
    bool log_io = true;           /* false: do not log write/pread (huge runs) */

    void put(const std::string &p, const Bytes &b) {
        auto f = std::make_shared<File>();
        f->data = b;
        f->size = b.size();
        f->mtime = ++clock;
        files[p] = f;
    }
    void put_virtual(const std::string &p, uint64_t size, std::function<void(uint64_t, unsigned char *, size_t)> g) {
        auto f = std::make_shared<File>();
        f->size = size;
        f->gen = std::move(g);
        f->mtime = ++clock;
        files[p] = f;
    }
    bool exists(const std::string &p) const { return files.count(p) != 0; }
    Bytes get(const std::string &p) const {
        auto it = files.find(p);
        return it == files.end() ? Bytes() : it->second->data;
    }
    std::vector<std::string> names(const std::string &dir) const {
        std::vector<std::string> v;
        for (const auto &kv : files) {
            const std::string &k = kv.first;
            if (k.size() > dir.size() + 1 && k.compare(0, dir.size(), dir) == 0 && k[dir.size()] == '/' &&
                k.find('/', dir.size() + 1) == std::string::npos)
                v.push_back(k.substr(dir.size() + 1));
        }
        return v;
    }
    std::vector<std::string> events(const std::string &prefix) const {
        std::vector<std::string> v;
        for (const std::string &e : log)
            if (e.compare(0, prefix.size(), prefix) == 0) v.push_back(e);
        return v;
    }

    int open(const std::string &p, int flags, int) override {
        int e;
        if (!gate("open " + p, e)) return e;
        auto it = files.find(p);
        if (it == files.end()) {
            if (!(flags & O_CREAT)) return -ENOENT;
            auto f = std::make_shared<File>();
            f->mtime = ++clock;
            it = files.emplace(p, f).first;
        } else {
            if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
            if ((flags & O_TRUNC) && !it->second->gen) {
                it->second->data.clear();
                it->second->size = 0;
            }
        }
        const int fd = next_fd++;
        fds[fd] = Fd{it->second, p, (flags & O_ACCMODE) != O_RDONLY, 0};
        return fd;
    }
    int close(int fd) override {
        if (dead) return -EIO;
        return fds.erase(fd) ? 0 : -EBADF;
    }
    ssize_t write(int fd, const void *b, size_t n) override {
        int e;
        auto it = fds.find(fd);
        const bool torn_ok = tear_writes && it != fds.end();
        if (!gate_w("write " + (it != fds.end() ? it->second.path : std::string("?")), e)) {
            if (killed_now && torn_ok && n > 1) store(*it->second.f, it->second.pos, b, n / 2);
            return e;
        }
        if (it == fds.end() || !it->second.wr) return -EBADF;
        store(*it->second.f, it->second.pos, b, n);
        it->second.pos += n;
        return static_cast<ssize_t>(n);
    }
    ssize_t pwrite(int fd, const void *b, size_t n, uint64_t off) override {
        int e;
        auto it = fds.find(fd);
        if (!gate("pwrite " + (it != fds.end() ? it->second.path : std::string("?")), e)) return e;
        if (it == fds.end() || !it->second.wr) return -EBADF;
        store(*it->second.f, off, b, n);
        return static_cast<ssize_t>(n);
    }
    ssize_t pread(int fd, void *b, size_t n, uint64_t off) override {
        int e;
        auto it = fds.find(fd);
        if (!gate_r("pread " + (it != fds.end() ? it->second.path : std::string("?")), e)) return e;
        if (it == fds.end()) return -EBADF;
        File &f = *it->second.f;
        max_pread = std::max(max_pread, n);
        if (off >= f.size) return 0;
        n = static_cast<size_t>(std::min<uint64_t>(n, f.size - off));
        if (f.gen) f.gen(off, static_cast<unsigned char *>(b), n);
        else {
            std::memset(b, 0, n);
            if (off < f.data.size()) std::memcpy(b, &f.data[static_cast<size_t>(off)], std::min<size_t>(n, f.data.size() - static_cast<size_t>(off)));
        }
        return static_cast<ssize_t>(n);
    }
    int fdatasync(int fd) override { return sync_op("fdatasync", fd); }
    int fsync(int fd) override { return sync_op("fsync", fd); }
    int ftruncate(int fd, uint64_t len) override {
        int e;
        auto it = fds.find(fd);
        if (!gate("ftruncate", e)) return e;
        if (it == fds.end()) return -EBADF;
        it->second.f->size = len;
        if (!it->second.f->discard) it->second.f->data.resize(static_cast<size_t>(len));
        return 0;
    }
    int fstat(int fd, StatInfo &st) override {
        if (dead) return -EIO;
        auto it = fds.find(fd);
        if (it == fds.end()) return -EBADF;
        st.size = it->second.f->size;
        st.mtime = it->second.f->mtime;
        return 0;
    }
    int stat(const std::string &p, StatInfo &st) override {
        if (dead) return -EIO;
        auto it = files.find(p);
        if (it == files.end()) return -ENOENT;
        st.size = it->second->size;
        st.mtime = it->second->mtime;
        return 0;
    }
    int rename(const std::string &a, const std::string &b) override {
        int e;
        if (!gate("rename " + a + " " + b, e)) return e;
        auto it = files.find(a);
        if (it == files.end()) return -ENOENT;
        std::shared_ptr<File> f = it->second;
        files.erase(it);
        files[b] = f;
        return 0;
    }
    int unlink(const std::string &p) override {
        int e;
        if (!gate("unlink " + p, e)) return e;
        return files.erase(p) ? 0 : -ENOENT;
    }
    int mkdir(const std::string &) override { return 0; }
    int fsync_dir(const std::string &p) override {
        int e;
        if (!gate("fsync_dir " + p, e)) return e;
        return 0;
    }
    int list_dir(const std::string &p, std::vector<std::string> &out) override {
        if (dead) return -EIO;
        for (const std::string &n : names(p)) out.push_back(n);
        return 0;
    }
    int free_space(const std::string &, uint64_t &avail) override {
        avail = free_bytes;
        return 0;
    }

    /* Make every discarding output: new files are created with discard set (for mocked huge results). */
    bool discard_new = false;

private:
    struct Fd {
        std::shared_ptr<File> f;
        std::string path;
        bool wr = false;
        uint64_t pos = 0;
    };
    std::map<int, Fd> fds;
    int next_fd = 3;
    bool killed_now = false;

    bool gate_core(const std::string &desc, int &err, bool logit) {
        killed_now = false;
        if (dead) {
            err = -EIO;
            return false;
        }
        ++step;
        if (step == kill_at) {
            dead = true;
            killed_now = true;
            err = -EIO;
            return false;
        }
        if (logit) log.push_back(desc);
        if (step == fail_at) {
            err = -fail_errno;
            return false;
        }
        return true;
    }
    bool gate(const std::string &d, int &e) { return gate_core(d, e, true); }
    bool gate_w(const std::string &d, int &e) { return gate_core(d, e, log_io); }
    bool gate_r(const std::string &d, int &e) { return gate_core(d, e, log_io); }

    int sync_op(const char *op, int fd) {
        int e;
        auto it = fds.find(fd);
        if (!gate(std::string(op) + " " + (it != fds.end() ? it->second.path : std::string("?")), e)) return e;
        return it == fds.end() ? -EBADF : 0;
    }
    void store(File &f, uint64_t off, const void *b, size_t n) {
        if (f.gen) return;
        f.mtime = ++clock;
        if (off + n > f.size) f.size = off + n;
        const unsigned char *p = static_cast<const unsigned char *>(b);
        if (discard_new && !f.discard && f.data.empty()) f.discard = true;
        if (f.discard) {
            if (off < kCap) {
                const size_t k = static_cast<size_t>(std::min<uint64_t>(n, kCap - off));
                if (f.data.size() < off + k) f.data.resize(static_cast<size_t>(off) + k);
                std::memcpy(&f.data[static_cast<size_t>(off)], p, k);
            }
            return;
        }
        if (f.data.size() < off + n) f.data.resize(static_cast<size_t>(off) + n);
        std::memcpy(&f.data[static_cast<size_t>(off)], p, n);
    }
};

/* ---------------------------------------------------------------- the name provider fake */

/* "take_006_20261108-1105.wav" -> -1.wav, -2.wav ... : the next free numbers, never reusing one it gave before. */
struct FakeNames {
    std::set<int> used;
    std::vector<std::string> calls;
    std::vector<std::string> operator()(const std::string &source, int count) {
        calls.push_back(source);
        std::string stem = source;
        const size_t dot = stem.rfind('.');
        if (dot != std::string::npos) stem.resize(dot);
        std::vector<std::string> out;
        int n = 1;
        for (int i = 0; i < count; i++) {
            while (used.count(n)) n++;
            used.insert(n);
            out.push_back(stem + "-" + std::to_string(n) + ".wav");
        }
        return out;
    }
};

/* ---------------------------------------------------------------- invariants after a crash */

struct Expected {
    std::string dir, source_name;
    Bytes original;                          /* the source file as it was */
    std::vector<std::string> result_names;   /* in part order; for a single-part replace: the source name */
    std::vector<Bytes> result_audio;         /* the audio bytes each result must hold */
    AudioFormat fmt;
    SaveMode mode = SaveMode::NewFiles;
    bool single_over = false;
};

/* Is `file` a complete, valid result holding exactly `audio`? */
inline bool valid_result(const Bytes &file, const Bytes &audio, const AudioFormat &fmt) {
    HeaderInfo h = parse_header(file.data(), std::min<size_t>(file.size(), 65536), file.size());
    if (!h.ok || !h.consistent()) return false;
    if (h.fmt.rate != fmt.rate || h.fmt.channels != fmt.channels || h.fmt.bits != fmt.bits) return false;
    if (h.data_offset != kDataOffset || h.actual_data != audio.size()) return false;
    return file.size() == kDataOffset + audio.size() &&
           std::equal(audio.begin(), audio.end(), file.begin() + static_cast<std::ptrdiff_t>(kDataOffset));
}

/* A view of a folder: names and file contents (the in-memory or the real one). */
struct FsView {
    std::function<std::vector<std::string>()> list;
    std::function<Bytes(const std::string &)> read;
};

/* Returns "" when the folder is in an allowed state, else the problem. allow_tmp: before cleanup_temporaries(). */
inline std::string check_state(const FsView &fs, const Expected &x, bool allow_tmp) {
    std::set<std::string> names;
    for (const std::string &n : fs.list()) names.insert(n);
    std::string bad;
    size_t results_present = 0;
    for (const std::string &n : names) {
        if (n.compare(0, 5, ".tmp-") == 0) {
            if (!allow_tmp) bad += "temporary left: " + n + "; ";
            continue;
        }
        if (n.size() > 6 && n.compare(n.size() - 6, 6, ".peaks") == 0) continue;
        if (n == x.source_name) continue; /* judged below */
        const auto it = std::find(x.result_names.begin(), x.result_names.end(), n);
        if (it == x.result_names.end()) {
            bad += "unexpected file " + n + "; ";
            continue;
        }
        const size_t i = static_cast<size_t>(it - x.result_names.begin());
        if (!valid_result(fs.read(n), x.result_audio[i], x.fmt)) bad += "half-written result under its final name: " + n + "; ";
        else results_present++;
    }
    const bool have_src = names.count(x.source_name) != 0;
    if (x.single_over) {
        if (!have_src) return bad + "the original name vanished";
        const Bytes cur = fs.read(x.source_name);
        if (cur != x.original && !valid_result(cur, x.result_audio[0], x.fmt))
            bad += "the original name holds neither the original nor the finished result; ";
        return bad;
    }
    if (have_src) {
        if (fs.read(x.source_name) != x.original) bad += "the original changed; ";
    } else {
        if (x.mode == SaveMode::NewFiles) bad += "the original is gone in new-files mode; ";
        else {
            /* replace: the original may go only when every result exists */
            for (const std::string &n : x.result_names)
                if (!names.count(n)) bad += "the original is gone but result " + n + " is missing; ";
        }
    }
    (void)results_present;
    return bad;
}

inline FsView mem_view(MemOps &m, const std::string &dir) {
    FsView v;
    v.list = [&m, dir] { return m.names(dir); };
    v.read = [&m, dir](const std::string &n) { return m.get(dir + "/" + n); };
    return v;
}

}  // namespace tst
