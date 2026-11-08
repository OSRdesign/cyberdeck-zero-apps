/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: crash safety. (1) every file operation of a save is made to fail, and to be the point of a kill, on an
 * in-memory file system; (2) a forked child doing a throttled large save is killed with SIGKILL at fixed pseudo-random times on
 * the real file system. After each: the original is byte-identical, or every result is complete; never a half file under a
 * final name; cleanup_temporaries() leaves no temporary.
 */
#include "check.hpp"
#include "helpers.hpp"
#include "peaks.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace tst;

namespace {

const char *kDir = "/rec";
const char *kSrcName = "take_006_20261108-1105.wav";

struct Scen {
    const char *name;
    uint64_t frames, in, out;
    std::vector<uint64_t> splits;
    SaveMode mode;
    uint16_t channels = 1, bits = 16;
};

Expected make_expected(const Scen &sc, const Bytes &src, const std::string &dir) {
    Expected x;
    x.dir = dir;
    x.source_name = kSrcName;
    x.original = src;
    x.fmt.channels = sc.channels;
    x.fmt.bits = sc.bits;
    x.mode = sc.mode;
    const size_t ba = sc.channels * (sc.bits / 8u);
    std::vector<uint64_t> cuts{sc.in};
    cuts.insert(cuts.end(), sc.splits.begin(), sc.splits.end());
    cuts.push_back(sc.out);
    x.single_over = sc.mode == SaveMode::ReplaceOriginal && cuts.size() == 2;
    FakeNames nm;
    std::vector<std::string> names = nm(kSrcName, static_cast<int>(cuts.size() - 1));
    for (size_t i = 0; i + 1 < cuts.size(); i++) {
        x.result_names.push_back(x.single_over ? std::string(kSrcName) : names[i]);
        const size_t o = kDataOffset + static_cast<size_t>(cuts[i]) * ba;
        x.result_audio.emplace_back(src.begin() + static_cast<std::ptrdiff_t>(o),
                                    src.begin() + static_cast<std::ptrdiff_t>(o + (cuts[i + 1] - cuts[i]) * ba));
    }
    return x;
}

struct Outcome {
    int steps = 0;
    bool ok = false;
    std::string problems;
};

Outcome run_scen(const Scen &sc, int kill_at, int fail_at, int fail_errno = EIO) {
    SrcSpec s;
    s.fmt.channels = sc.channels;
    s.fmt.bits = sc.bits;
    s.frames = sc.frames;
    const Bytes src = make_source(s);
    MemOps m;
    const std::string path = std::string(kDir) + "/" + kSrcName;
    m.put(path, src);
    m.put(path + ".peaks", Bytes(40, 1));
    m.kill_at = kill_at;
    m.fail_at = fail_at;
    m.fail_errno = fail_errno;
    FakeNames nm;
    EditOptions o;
    o.ops = &m;
    o.names = [&](const std::string &n, int c) { return nm(n, c); };
    o.block_bytes = 1024; /* many write steps */
    EditRequest r;
    r.source_path = path;
    r.in_frame = sc.in;
    r.out_frame = sc.out;
    r.splits = sc.splits;
    r.mode = sc.mode;
    EditResult res = trim_split(r, o);

    Outcome out;
    out.steps = m.step;
    out.ok = res.ok;
    const Expected x = make_expected(sc, src, kDir);
    const FsView v = mem_view(m, kDir);
    out.problems = check_state(v, x, true);
    /* what the file system holds when the process is gone; then the next start cleans up */
    m.dead = false;
    m.kill_at = -1;
    m.fail_at = -1;
    cleanup_temporaries(m, kDir);
    const std::string after = check_state(v, x, false);
    if (!after.empty()) out.problems += "after cleanup: " + after;
    if (kill_at < 0 && fail_at < 0 && res.ok) { /* a clean run must have produced everything */
        for (const std::string &n : x.result_names)
            if (!m.exists(std::string(kDir) + "/" + n)) out.problems += "clean run: missing " + n + "; ";
        if (sc.mode == SaveMode::ReplaceOriginal && !x.single_over && m.exists(path)) out.problems += "clean replace kept the original; ";
    }
    return out;
}

const Scen kScens[] = {
    {"trim, new file", 3000, 100, 2900, {}, SaveMode::NewFiles},
    {"split 2 points, new files", 3000, 0, 3000, {1000, 2000}, SaveMode::NewFiles},
    {"split 3 points + trim, new files, stereo S24", 2000, 50, 1950, {500, 900, 1500}, SaveMode::NewFiles, 2, 24},
    {"split 2 points, replace", 3000, 10, 2990, {1000, 2000}, SaveMode::ReplaceOriginal},
    {"trim alone, replace (rename over the original)", 3000, 100, 2900, {}, SaveMode::ReplaceOriginal},
};

void t_every_step() {
    for (const Scen &sc : kScens) {
        const Outcome base = run_scen(sc, -1, -1);
        CHECK(base.ok);
        CHECK(base.problems.empty());
        if (!base.problems.empty()) std::fprintf(stderr, "[%s] clean run: %s\n", sc.name, base.problems.c_str());
        CHECK(base.steps > 12);
        int kills_with_tmp = 0, kills_with_final = 0;
        for (int n = 1; n <= base.steps + 1; n++) {
            /* kill (SIGKILL: the call does not happen, nothing runs after it) */
            Outcome k = run_scen(sc, n, -1);
            if (!k.problems.empty()) std::fprintf(stderr, "[%s] kill at step %d: %s\n", sc.name, n, k.problems.c_str());
            CHECK(k.problems.empty());
            (void)kills_with_tmp;
            (void)kills_with_final;
            /* a failing call, once, with EIO and with ENOSPC; the engine carries on and cleans up */
            for (int err : {EIO, ENOSPC}) {
                Outcome f = run_scen(sc, -1, n, err);
                if (!f.problems.empty()) std::fprintf(stderr, "[%s] fail at step %d (errno %d): %s\n", sc.name, n, err, f.problems.c_str());
                CHECK(f.problems.empty());
                if (n <= base.steps && sc.mode == SaveMode::NewFiles && !f.ok) {
                    /* a clean failure: the original is there and unchanged (check_state said so) */
                    CHECK(true);
                }
            }
        }
    }
}

/* Every point of a kill during a peak build: a cache is either absent or valid, and the next open is correct. */
void t_peaks_every_step() {
    SrcSpec s;
    s.frames = 5000;
    const Bytes src = make_source(s);
    int T = 0;
    for (int n = 0; n <= 100; n++) {
        MemOps m;
        m.put("/rec/p.wav", src);
        m.kill_at = n == 0 ? -1 : n;
        std::string err;
        {
            auto P = PeakPyramid::open_or_build(m, "/rec/p.wav", &err);
        }
        if (n == 0) T = m.step;
        const bool killed = m.dead;
        m.dead = false;
        m.kill_at = -1;
        cleanup_temporaries(m, "/rec");
        for (const std::string &nm : m.names("/rec")) CHECK(nm.compare(0, 5, ".tmp-") != 0);
        auto P = PeakPyramid::open_or_build(m, "/rec/p.wav", &err);
        CHECK(P != nullptr);
        if (P) {
            CHECK_EQ(P->frames(), 5000u);
            std::vector<PeakPair> w = P->window(0, 5000, 1);
            int lo = 99999, hi = -99999;
            for (uint64_t i = 0; i < 5000; i++) {
                lo = std::min(lo, sample16(i, 0));
                hi = std::max(hi, sample16(i, 0));
            }
            CHECK_EQ(static_cast<int>(w[0].min), lo);
            CHECK_EQ(static_cast<int>(w[0].max), hi);
            CHECK(m.get("/rec/p.wav") == src);
        }
        if (!killed && n > 0) break;
    }
    CHECK(T > 5);
}

/* ---------------------------------------------------------------- forked SIGKILL on the real file system */

class SlowOps final : public FileOps {
public:
    FileOps &b = posix_file_ops();
    int open(const std::string &p, int f, int m) override { return b.open(p, f, m); }
    int close(int fd) override { return b.close(fd); }
    ssize_t write(int fd, const void *x, size_t n) override {
        usleep(100); /* the SD card: a large copy takes long enough to be hit anywhere */
        return b.write(fd, x, n);
    }
    ssize_t pwrite(int fd, const void *x, size_t n, uint64_t o) override { return b.pwrite(fd, x, n, o); }
    ssize_t pread(int fd, void *x, size_t n, uint64_t o) override { return b.pread(fd, x, n, o); }
    int fdatasync(int fd) override { return b.fdatasync(fd); }
    int fsync(int fd) override { return b.fsync(fd); }
    int ftruncate(int fd, uint64_t l) override { return b.ftruncate(fd, l); }
    int fstat(int fd, StatInfo &s) override { return b.fstat(fd, s); }
    int stat(const std::string &p, StatInfo &s) override { return b.stat(p, s); }
    int rename(const std::string &x, const std::string &y) override {
        usleep(200);
        return b.rename(x, y);
    }
    int unlink(const std::string &p) override { return b.unlink(p); }
    int mkdir(const std::string &p) override { return b.mkdir(p); }
    int fsync_dir(const std::string &p) override { return b.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return b.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override { return b.free_space(p, a); }
};

Bytes slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void t_fork_kill() {
    const uint64_t frames = 12ull * 1000 * 1000; /* 24 MB of mono S16 */
    SrcSpec s;
    s.frames = frames;
    const Bytes src = make_source(s);
    const Scen scs[] = {
        {"fork trim", frames, 1000, frames - 1000, {}, SaveMode::NewFiles},
        {"fork split 3", frames, 0, frames, {4000000, 8000000}, SaveMode::NewFiles},
        {"fork split replace", frames, 5, frames - 5, {3000000, 9000000}, SaveMode::ReplaceOriginal},
        {"fork trim replace", frames, 2000, frames - 3000, {}, SaveMode::ReplaceOriginal},
    };
    const unsigned seeds[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89};
    int killed_with_tmp = 0, finished = 0, runs = 0, with_final = 0;
    for (const Scen &sc : scs) {
        for (unsigned seed : seeds) {
            char tmpl[] = "/tmp/cassette-fork-XXXXXX";
            const std::string dir = mkdtemp(tmpl);
            const std::string path = dir + "/" + kSrcName;
            {
                std::ofstream f(path, std::ios::binary);
                f.write(reinterpret_cast<const char *>(src.data()), static_cast<std::streamsize>(src.size()));
            }
            const useconds_t delay_us = static_cast<useconds_t>(mix(seed * 1000003ull + frames) % 120000); /* 0..120 ms */
            pid_t pid = fork();
            if (pid == 0) {
                SlowOps ops;
                FakeNames nm;
                EditOptions o;
                o.ops = &ops;
                o.names = [&](const std::string &n, int c) { return nm(n, c); };
                o.block_bytes = 64 * 1024;
                EditRequest r;
                r.source_path = path;
                r.in_frame = sc.in;
                r.out_frame = sc.out;
                r.splits = sc.splits;
                r.mode = sc.mode;
                EditResult res = trim_split(r, o);
                _exit(res.ok ? 0 : 3);
            }
            usleep(delay_us);
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
            const bool exited = WIFEXITED(status);
            Expected x = make_expected(sc, src, dir);
            FsView v;
            v.list = [&dir] {
                std::vector<std::string> n;
                posix_file_ops().list_dir(dir, n);
                return n;
            };
            v.read = [&dir](const std::string &n) { return slurp(dir + "/" + n); };
            std::vector<std::string> names = v.list();
            bool had_tmp = false, had_final = false;
            for (const std::string &n : names) {
                had_tmp = had_tmp || n.compare(0, 5, ".tmp-") == 0;
                had_final = had_final || n.find("-1.wav") != std::string::npos;
            }
            const std::string p1 = check_state(v, x, true);
            if (!p1.empty()) std::fprintf(stderr, "[%s seed %u delay %u us] %s\n", sc.name, seed, static_cast<unsigned>(delay_us), p1.c_str());
            CHECK(p1.empty());
            const int removed = cleanup_temporaries(dir);
            CHECK(removed >= 0);
            const std::string p2 = check_state(v, x, false);
            if (!p2.empty()) std::fprintf(stderr, "[%s seed %u] after cleanup: %s\n", sc.name, seed, p2.c_str());
            CHECK(p2.empty());
            runs++;
            killed_with_tmp += (!exited && had_tmp) ? 1 : 0;
            finished += exited ? 1 : 0;
            with_final += had_final ? 1 : 0;
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
    std::printf("  fork+SIGKILL: %d runs, %d killed with a temporary present, %d finished before the kill, %d with a result in place\n",
                runs, killed_with_tmp, finished, with_final);
}

}  // namespace

void test_crash_steps() {
    t_every_step();
    t_peaks_every_step();
}
void test_crash_fork() { t_fork_kill(); }
