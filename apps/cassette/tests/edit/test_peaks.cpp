/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette edit tests: the peak pyramid (brute-force reference at several zooms, stale and damaged caches, placeholder
 * headers, formats, bounded reads on a mocked 256 MiB take, bounded work per window).
 */
#include "check.hpp"
#include "helpers.hpp"
#include "peaks.hpp"

#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

using namespace tst;

namespace {

/* The real file system with counters (reads) and one failure switch. */
class CountOps final : public FileOps {
public:
    FileOps &b = posix_file_ops();
    int preads = 0;
    uint64_t pread_bytes = 0;
    bool fail_rename = false;
    int open(const std::string &p, int f, int m) override { return b.open(p, f, m); }
    int close(int fd) override { return b.close(fd); }
    ssize_t write(int fd, const void *x, size_t n) override { return b.write(fd, x, n); }
    ssize_t pwrite(int fd, const void *x, size_t n, uint64_t o) override { return b.pwrite(fd, x, n, o); }
    ssize_t pread(int fd, void *x, size_t n, uint64_t o) override {
        preads++;
        pread_bytes += n;
        return b.pread(fd, x, n, o);
    }
    int fdatasync(int fd) override { return b.fdatasync(fd); }
    int fsync(int fd) override { return b.fsync(fd); }
    int ftruncate(int fd, uint64_t l) override { return b.ftruncate(fd, l); }
    int fstat(int fd, StatInfo &s) override { return b.fstat(fd, s); }
    int stat(const std::string &p, StatInfo &s) override { return b.stat(p, s); }
    int rename(const std::string &x, const std::string &y) override { return fail_rename ? -EACCES : b.rename(x, y); }
    int unlink(const std::string &p) override { return b.unlink(p); }
    int mkdir(const std::string &p) override { return b.mkdir(p); }
    int fsync_dir(const std::string &p) override { return b.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return b.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override { return b.free_space(p, a); }
};

struct Dir {
    std::string path;
    Dir() {
        char t[] = "/tmp/cassette-peaks-XXXXXX";
        path = mkdtemp(t);
    }
    ~Dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string &n) const { return path + "/" + n; }
    std::vector<std::string> list() const {
        std::vector<std::string> v;
        posix_file_ops().list_dir(path, v);
        return v;
    }
};

void write_bytes(const std::string &p, const Bytes &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

Bytes read_all(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* Per-frame min and max over the channels of the synthetic signal (16-bit scale): the brute-force reference. */
struct Ref {
    std::vector<int> mn, mx;
    uint64_t frames = 0;
    Ref(const AudioFormat &f, uint64_t frames_, uint64_t first = 0) : frames(frames_) {
        mn.resize(static_cast<size_t>(frames));
        mx.resize(static_cast<size_t>(frames));
        for (uint64_t i = 0; i < frames; i++) {
            int lo = 99999, hi = -99999;
            for (unsigned c = 0; c < f.channels; c++) {
                const int v = sample16(first + i, c);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            mn[static_cast<size_t>(i)] = lo;
            mx[static_cast<size_t>(i)] = hi;
        }
    }
    PeakPair range(uint64_t a, uint64_t b) const {
        int lo = 99999, hi = -99999;
        for (uint64_t i = a; i < b; i++) {
            lo = std::min(lo, mn[static_cast<size_t>(i)]);
            hi = std::max(hi, mx[static_cast<size_t>(i)]);
        }
        PeakPair p;
        p.min = static_cast<int16_t>(lo);
        p.max = static_cast<int16_t>(hi);
        return p;
    }
};

/* Compares window() with the reference: exact below 256 frames per bar, block-extent above (and a superset of the exact). */
int check_window(PeakPyramid &P, const Ref &ref, uint64_t start, uint64_t fpb, size_t bars) {
    int bad = 0;
    std::vector<PeakPair> res = P.window(start, fpb, bars);
    if (res.size() != bars) return 1;
    const uint64_t F = ref.frames;
    const size_t k = P.level_for(fpb);
    const uint64_t bs = static_cast<uint64_t>(kPeakBlock) << k;
    for (size_t i = 0; i < bars; i++) {
        const uint64_t s = start + i * fpb;
        PeakPair want;
        if (s < F) {
            const uint64_t e = std::min(s + fpb, F);
            if (fpb < kPeakBlock) want = ref.range(s, e);
            else {
                const uint64_t b0 = s / bs * bs, b1 = std::min((e + bs - 1) / bs * bs, F);
                want = ref.range(b0, b1);
                const PeakPair exact = ref.range(s, e);
                if (!(res[i].min <= exact.min && res[i].max >= exact.max)) bad++;
            }
        }
        if (!(res[i] == want)) bad++;
    }
    return bad;
}

const uint64_t kF = 300000;

void zoom_suite(PeakPyramid &P, const Ref &ref, uint64_t F) {
    const uint64_t fpbs[] = {1, 3, 100, 255, 256, 257, 511, 512, 1000, 4096, 20000, 100000, 1ull << 20, F, F * 2};
    const uint64_t starts[] = {0, 12345, F / 2, F - 50, F, F + 10};
    int bad = 0;
    for (uint64_t fpb : fpbs)
        for (uint64_t st : starts)
            for (size_t bars : {size_t(1), size_t(7), size_t(300)}) bad += check_window(P, ref, st, fpb, bars);
    CHECK_EQ(bad, 0);
    /* aligned bars are exact at every level */
    bad = 0;
    for (size_t k = 0; k < P.levels(); k++) {
        const uint64_t bs = static_cast<uint64_t>(kPeakBlock) << k;
        if (bs >= F) break;
        std::vector<PeakPair> w = P.window(0, bs, 5);
        for (size_t i = 0; i < 5; i++)
            if (i * bs < F && !(w[i] == ref.range(i * bs, std::min((i + 1) * bs, F)))) bad++;
    }
    CHECK_EQ(bad, 0);
}

void t_zoom_mono() {
    Dir d;
    SrcSpec s;
    s.frames = kF;
    write_bytes(d.file("a.wav"), make_source(s));
    CountOps ops;
    std::string err;
    std::vector<uint64_t> prog;
    PeakBuildOptions bo;
    bo.progress = [&](uint64_t done, uint64_t) { prog.push_back(done); };
    std::unique_ptr<PeakPyramid> P = PeakPyramid::open_or_build(ops, d.file("a.wav"), &err, bo);
    CHECK(P != nullptr);
    if (!P) return;
    CHECK(P->rebuilt());
    CHECK_EQ(P->frames(), kF);
    CHECK_EQ(P->rate(), 48000u);
    CHECK_EQ(P->level_blocks(0), (kF + 255) / 256);
    CHECK_EQ(P->level_blocks(P->levels() - 1), 1u); /* up to a single block */
    CHECK_EQ(P->levels(), 12u);
    for (size_t k = 1; k < P->levels(); k++) CHECK_EQ(P->level_blocks(k), (P->level_blocks(k - 1) + 1) / 2); /* each halves */
    CHECK(!prog.empty() && prog.front() == 0 && prog.back() == kF);
    for (size_t i = 1; i < prog.size(); i++) CHECK(prog[i] >= prog[i - 1]);
    Ref ref(s.fmt, kF);
    zoom_suite(*P, ref, kF);
    /* the cache is next to the take, atomic: no temporaries left */
    bool cache = false, tmp = false;
    for (const std::string &n : d.list()) {
        cache = cache || n == "a.wav.peaks";
        tmp = tmp || n.compare(0, 5, ".tmp-") == 0;
    }
    CHECK(cache);
    CHECK(!tmp);
    /* the second open reads the cache: no rebuild */
    P.reset();
    P = PeakPyramid::open_or_build(ops, d.file("a.wav"), &err);
    CHECK(P && !P->rebuilt());
    if (P) zoom_suite(*P, ref, kF);
}

void t_formats() {
    struct F {
        uint16_t ch, bits;
    } fs[] = {{1, 16}, {2, 16}, {1, 24}, {2, 24}, {1, 32}, {2, 32}, {3, 16}};
    for (const F &f : fs) {
        Dir d;
        SrcSpec s;
        s.fmt.channels = f.ch;
        s.fmt.bits = f.bits;
        s.frames = 20000;
        write_bytes(d.file("t.wav"), make_source(s));
        CountOps ops;
        std::string err;
        auto P = PeakPyramid::open_or_build(ops, d.file("t.wav"), &err);
        CHECK(P != nullptr);
        if (!P) continue;
        Ref ref(s.fmt, s.frames); /* stereo: min of the channels' mins, max of their maxes */
        const uint64_t fpbs[] = {1, 50, 255, 256, 700, 5000, 20000};
        int bad = 0;
        for (uint64_t fpb : fpbs)
            for (uint64_t st : {uint64_t(0), uint64_t(777)}) bad += check_window(*P, ref, st, fpb, 40);
        CHECK_EQ(bad, 0);
    }
}

void t_small_and_empty() {
    for (uint64_t frames : {uint64_t(0), uint64_t(1), uint64_t(100), uint64_t(256), uint64_t(257), uint64_t(1025)}) {
        Dir d;
        SrcSpec s;
        s.frames = frames;
        write_bytes(d.file("s.wav"), make_source(s));
        CountOps ops;
        std::string err;
        auto P = PeakPyramid::open_or_build(ops, d.file("s.wav"), &err);
        CHECK(P != nullptr);
        if (!P) continue;
        CHECK_EQ(P->frames(), frames);
        if (frames == 0) {
            CHECK_EQ(P->levels(), 0u);
            std::vector<PeakPair> w = P->window(0, 1000, 5);
            CHECK_EQ(w.size(), 5u);
            for (const PeakPair &p : w) CHECK(p == PeakPair());
            continue;
        }
        Ref ref(s.fmt, frames);
        int bad = 0;
        for (uint64_t fpb : {uint64_t(1), uint64_t(100), uint64_t(256), uint64_t(1000), uint64_t(100000)})
            for (uint64_t st : {uint64_t(0), uint64_t(1), frames / 2, frames}) bad += check_window(*P, ref, st, fpb, 9);
        CHECK_EQ(bad, 0);
        CHECK_EQ(P->window(0, 0, 3).size(), 3u); /* fpb 0 is treated as 1 */
        CHECK(P->window(0, 10, 0).empty());
    }
}

void t_placeholder_header() {
    for (SrcSpec::Kind k : {SrcSpec::Placeholder, SrcSpec::Arecord44}) {
        Dir d;
        SrcSpec s;
        s.kind = k;
        s.frames = 50000;
        write_bytes(d.file("u.wav"), make_source(s));
        CountOps ops;
        std::string err;
        auto P = PeakPyramid::open_or_build(ops, d.file("u.wav"), &err);
        CHECK(P != nullptr);
        if (!P) continue;
        CHECK_EQ(P->frames(), 50000u); /* the real file length, not the placeholder */
        Ref ref(s.fmt, 50000);
        int bad = 0;
        for (uint64_t fpb : {uint64_t(10), uint64_t(256), uint64_t(3000)}) bad += check_window(*P, ref, 0, fpb, 60);
        CHECK_EQ(bad, 0);
    }
    { /* a damaged header: no pyramid, a reason */
        Dir d;
        write_bytes(d.file("x.wav"), Bytes(2000, 9));
        CountOps ops;
        std::string err;
        CHECK(PeakPyramid::open_or_build(ops, d.file("x.wav"), &err) == nullptr);
        CHECK(!err.empty());
        CHECK(PeakPyramid::open_or_build(ops, d.file("none.wav"), &err) == nullptr);
    }
    { /* a half sample at the end is not a frame */
        Dir d;
        SrcSpec s;
        s.fmt.channels = 2;
        s.frames = 1000;
        Bytes b = make_source(s);
        b.push_back(1);
        b.push_back(2);
        b.push_back(3);
        write_bytes(d.file("h.wav"), b);
        CountOps ops;
        std::string err;
        auto P = PeakPyramid::open_or_build(ops, d.file("h.wav"), &err);
        CHECK(P && P->frames() == 1000u);
    }
}

void t_cache_validity() {
    Dir d;
    SrcSpec s;
    s.frames = 40000;
    const std::string take = d.file("c.wav"), cache = take + ".peaks";
    write_bytes(take, make_source(s));
    CountOps ops;
    std::string err;
    Ref ref(s.fmt, 40000);
    auto good = [&](bool expect_rebuilt, const char *what) {
        auto P = PeakPyramid::open_or_build(ops, take, &err);
        if (!P || P->rebuilt() != expect_rebuilt) std::fprintf(stderr, "cache case: %s\n", what);
        CHECK(P != nullptr);
        if (!P) return;
        CHECK_EQ(P->rebuilt(), expect_rebuilt);
        int bad = 0;
        for (uint64_t fpb : {uint64_t(64), uint64_t(256), uint64_t(2000), uint64_t(40000)}) bad += check_window(*P, ref, 0, fpb, 25);
        CHECK_EQ(bad, 0);
    };
    good(true, "first build");
    good(false, "reuse");

    { /* a truncated cache is rejected and rebuilt */
        const uint64_t sz = std::filesystem::file_size(cache);
        std::filesystem::resize_file(cache, sz - 3);
        good(true, "truncated by 3");
        good(false, "reuse after truncation rebuild");
        std::filesystem::resize_file(cache, sz / 2);
        good(true, "truncated to half");
        std::filesystem::resize_file(cache, 10);
        good(true, "truncated into the header");
        std::filesystem::resize_file(cache, 0);
        good(true, "empty");
    }
    { /* longer than it should be */
        {
            std::ofstream f(cache, std::ios::binary | std::ios::app);
            f.write("xxxx", 4);
        }
        good(true, "too long");
    }
    for (size_t at : {size_t(0), size_t(4), size_t(8), size_t(24), size_t(40), size_t(52), size_t(64), size_t(70)}) {
        /* a flipped header byte: magic, version, take size, frames, rate, level count, level table */
        Bytes c = read_all(cache);
        c[at] ^= 0x5A;
        write_bytes(cache, c);
        good(true, ("flipped header byte " + std::to_string(at)).c_str());
    }
    write_bytes(cache, Bytes(5000, 0x41));
    good(true, "garbage");
    { /* stale: the take grew */
        SrcSpec s2 = s;
        s2.frames = 41000;
        write_bytes(take, make_source(s2));
        auto P = PeakPyramid::open_or_build(ops, take, &err);
        CHECK(P && P->rebuilt() && P->frames() == 41000u);
        s.frames = 41000;
        ref = Ref(s.fmt, 41000);
        good(false, "after growth");
    }
    { /* stale: same size, different time stamp (the take was rewritten) */
        struct utimbuf tb;
        tb.actime = tb.modtime = 1500000000;
        CHECK_EQ(utime(take.c_str(), &tb), 0);
        good(true, "mtime changed");
        good(false, "mtime reuse");
    }
    { /* stale: same size and time, different audio is NOT detectable (documented); the size/mtime pair is the key */
        good(false, "same key");
    }
    { /* no leftovers */
        for (const std::string &n : d.list()) CHECK(n.compare(0, 5, ".tmp-") != 0);
    }
}

void t_cancel_and_failure() {
    Dir d;
    SrcSpec s;
    s.frames = 200000;
    const std::string take = d.file("k.wav");
    write_bytes(take, make_source(s));
    CountOps ops;
    std::string err;
    std::atomic<bool> cancel(true);
    PeakBuildOptions bo;
    bo.cancel = &cancel;
    CHECK(PeakPyramid::open_or_build(ops, take, &err, bo) == nullptr);
    CHECK_EQ(err, "cancelled");
    CHECK_EQ(d.list().size(), 1u); /* no cache, no temporary */
    /* the cache cannot be stored: the pyramid is built in memory, same answers, nothing left in the folder */
    ops.fail_rename = true;
    {
        std::unique_ptr<PeakPyramid> M = PeakPyramid::open_or_build(ops, take, &err);
        CHECK(M != nullptr);
        if (M) {
            CHECK(M->in_memory());
            CHECK(M->rebuilt());
            Ref ref(s.fmt, s.frames);
            zoom_suite(*M, ref, s.frames);
        }
    }
    CHECK_EQ(d.list().size(), 1u);
    ops.fail_rename = false;
    CHECK(PeakPyramid::open_or_build(ops, take, &err) != nullptr);
    CHECK_EQ(d.list().size(), 2u);
}

void t_window_cost() {
    Dir d;
    SrcSpec s;
    s.frames = 600000;
    write_bytes(d.file("w.wav"), make_source(s));
    CountOps ops;
    std::string err;
    auto P = PeakPyramid::open_or_build(ops, d.file("w.wav"), &err);
    CHECK(P != nullptr);
    if (!P) return;
    for (uint64_t fpb : {uint64_t(2), uint64_t(100), uint64_t(256), uint64_t(1000), uint64_t(5000), uint64_t(100000), uint64_t(600000)}) {
        for (size_t bars : {size_t(100), size_t(480)}) {
            ops.preads = 0;
            ops.pread_bytes = 0;
            P->window(1000, fpb, bars);
            CHECK(ops.preads <= 1); /* one read of the needed part only */
            if (fpb >= kPeakBlock) CHECK(ops.pread_bytes <= (2 * bars + 3) * 4);
            else CHECK(ops.pread_bytes <= bars * fpb * 2);
        }
    }
    /* the 2 s window of a 48 kHz take at 480 bars is 200 frames per bar: still exact and one read */
    ops.preads = 0;
    P->window(48000, 200, 480);
    CHECK_EQ(ops.preads, 1);
}

/* A mocked 256 MiB take (generated on read): the build never reads more than a chunk at a time. */
void t_large_virtual() {
    MemOps m;
    m.log_io = false;
    AudioFormat fmt;
    const uint64_t frames = (256ull << 20) / 2;
    auto g = [](uint64_t f) { return static_cast<int>(((f * 2654435761ull) >> 13) & 0x7FFF) - 16384; };
    BwfInfo info;
    const Bytes head = build_header(fmt, info, frames * 2);
    const std::string path = "/rec/big.wav";
    m.put_virtual(path, kDataOffset + frames * 2, [head, g](uint64_t off, unsigned char *b, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const uint64_t pos = off + i;
            if (pos < kDataOffset) b[i] = head[static_cast<size_t>(pos)];
            else {
                const uint64_t q = pos - kDataOffset;
                b[i] = static_cast<unsigned char>(static_cast<uint32_t>(g(q / 2)) >> (8 * (q & 1)));
            }
        }
    });
    std::string err;
    auto P = PeakPyramid::open_or_build(m, path, &err);
    CHECK(P != nullptr);
    if (!P) return;
    CHECK_EQ(P->frames(), frames);
    CHECK(m.max_pread <= kPeakReadChunk);
    int lo = 99999, hi = -99999;
    for (uint64_t f = 0; f < frames; f++) {
        lo = std::min(lo, g(f));
        hi = std::max(hi, g(f));
    }
    std::vector<PeakPair> whole = P->window(0, frames, 1);
    CHECK_EQ(static_cast<int>(whole[0].min), lo);
    CHECK_EQ(static_cast<int>(whole[0].max), hi);
    /* an aligned window in the middle at the 2^20 level */
    std::vector<PeakPair> w = P->window(5ull << 20, 1ull << 20, 4);
    for (uint64_t i = 0; i < 4; i++) {
        int a = 99999, b = -99999;
        for (uint64_t f = (5 + i) << 20; f < (6 + i) << 20; f++) {
            a = std::min(a, g(f));
            b = std::max(b, g(f));
        }
        CHECK_EQ(static_cast<int>(w[i].min), a);
        CHECK_EQ(static_cast<int>(w[i].max), b);
    }
    /* the cache holds ~4 bytes per 256 frames: about 1.6% of the take's samples, written next to it */
    StatInfo st;
    CHECK_EQ(m.stat(path + ".peaks", st), 0);
    CHECK(st.size > 0 && st.size < frames * 2 / 50);
}

}  // namespace

void test_peaks() {
    t_zoom_mono();
    t_formats();
    t_small_and_empty();
    t_placeholder_header();
    t_cache_validity();
    t_cancel_and_failure();
    t_window_cost();
    t_large_virtual();
}
