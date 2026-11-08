/*
 * SPDX-License-Identifier: MIT
 */
#include "peaks.hpp"

#include "edit_util.hpp"
#include "wav.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>

namespace cassette {
namespace {

using namespace editutil;

constexpr uint32_t kMagic = 0x534B5043; /* "CPKS" */
constexpr uint32_t kVersion = 1;
constexpr size_t kFixedHeader = 64;
constexpr size_t kPairBytes = 4;

struct Layout {
    std::vector<uint64_t> counts, offsets;
    uint64_t total = 0; /* the whole cache file */
};

Layout compute_layout(uint64_t frames) {
    Layout l;
    uint64_t c = (frames + kPeakBlock - 1) / kPeakBlock;
    if (c > 0) {
        l.counts.push_back(c);
        while (c > 1) {
            c = (c + 1) / 2;
            l.counts.push_back(c);
        }
    }
    uint64_t off = kFixedHeader + 16 * l.counts.size();
    for (uint64_t n : l.counts) {
        l.offsets.push_back(off);
        off += n * kPairBytes;
    }
    l.total = off;
    return l;
}

struct Meta {
    uint64_t take_size = 0;
    int64_t take_mtime = 0;
    uint64_t frames = 0, data_offset = 0;
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t block_align = 0;
};

std::vector<unsigned char> encode_header(const Meta &m, const Layout &l) {
    std::vector<unsigned char> h(kFixedHeader + 16 * l.counts.size(), 0);
    put32(&h[0], kMagic);
    put32(&h[4], kVersion);
    put64(&h[8], m.take_size);
    put64(&h[16], static_cast<uint64_t>(m.take_mtime));
    put64(&h[24], m.frames);
    put64(&h[32], m.data_offset);
    put32(&h[40], m.rate);
    put16(&h[44], m.channels);
    put16(&h[46], m.bits);
    put32(&h[48], kPeakBlock);
    put32(&h[52], static_cast<uint32_t>(l.counts.size()));
    for (size_t i = 0; i < l.counts.size(); i++) {
        put64(&h[kFixedHeader + 16 * i], l.offsets[i]);
        put64(&h[kFixedHeader + 16 * i + 8], l.counts[i]);
    }
    return h;
}

/* A cache is valid when its header equals the one we would write for this take and its length is exact. */
bool cache_valid(FileOps &ops, int fd, const Meta &m, const Layout &l) {
    StatInfo st;
    if (ops.fstat(fd, st) < 0 || st.size != l.total) return false;
    const std::vector<unsigned char> want = encode_header(m, l);
    std::vector<unsigned char> got(want.size());
    if (pread_full(ops, fd, got.data(), got.size(), 0) != static_cast<int64_t>(got.size())) return false;
    return got == want;
}

void store_pair(unsigned char *p, int mn, int mx) {
    put16(p, static_cast<uint32_t>(static_cast<uint16_t>(mn)));
    put16(p + 2, static_cast<uint32_t>(static_cast<uint16_t>(mx)));
}
PeakPair load_pair(const unsigned char *p) {
    PeakPair r;
    r.min = static_cast<int16_t>(get16(p));
    r.max = static_cast<int16_t>(get16(p + 2));
    return r;
}

/* min/max of n interleaved samples of `bytes` bytes each (16-bit scale). All channels together = the envelope. */
void scan(const unsigned char *p, size_t n, unsigned bytes, int &mn, int &mx) {
    mn = 32767;
    mx = -32768;
    for (size_t i = 0; i < n; i++, p += bytes) {
        int v;
        if (bytes == 2) v = static_cast<int16_t>(get16(p));
        else if (bytes == 3)
            v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                     static_cast<uint32_t>(p[2]) << 24) >> 16;
        else v = static_cast<int32_t>(get32(p)) >> 16;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
}

int fill(FileOps &ops, int fd, int take_fd, const Meta &m, const Layout &l, const PeakBuildOptions &opt) {
    const std::vector<unsigned char> hdr = encode_header(m, l);
    int r = pwrite_all(ops, fd, hdr.data(), hdr.size(), 0);
    if (r < 0) return r;
    if (l.counts.empty()) return 0;
    const unsigned sb = m.bits / 8u;
    const size_t chunk_frames = std::max<size_t>(kPeakBlock, (kPeakReadChunk / m.block_align / kPeakBlock) * kPeakBlock);
    std::vector<unsigned char> buf(chunk_frames * m.block_align);
    std::vector<unsigned char> out((chunk_frames / kPeakBlock) * kPairBytes);
    uint64_t done = 0;
    if (opt.progress) opt.progress(0, m.frames);
    while (done < m.frames) {
        if (opt.cancel && opt.cancel->load()) return -ECANCELED;
        const uint64_t want = std::min<uint64_t>(chunk_frames, m.frames - done);
        const size_t bytes = static_cast<size_t>(want) * m.block_align;
        int64_t got = pread_full(ops, take_fd, buf.data(), bytes, m.data_offset + done * m.block_align);
        if (got < 0) return static_cast<int>(got);
        if (static_cast<size_t>(got) != bytes) return -EIO; /* the take shrank under us */
        const size_t nblocks = static_cast<size_t>((want + kPeakBlock - 1) / kPeakBlock);
        for (size_t b = 0; b < nblocks; b++) {
            const size_t f0 = b * kPeakBlock;
            const size_t nf = static_cast<size_t>(std::min<uint64_t>(kPeakBlock, want - f0));
            int mn, mx;
            scan(&buf[f0 * m.block_align], nf * m.channels, sb, mn, mx);
            store_pair(&out[b * kPairBytes], mn, mx);
        }
        r = pwrite_all(ops, fd, out.data(), nblocks * kPairBytes, l.offsets[0] + (done / kPeakBlock) * kPairBytes);
        if (r < 0) return r;
        done += want;
        if (opt.progress) opt.progress(done, m.frames);
    }
    /* the upper levels, streamed from the level below */
    const size_t in_pairs = 1u << 15; /* even, so chunks pair up cleanly */
    std::vector<unsigned char> in(in_pairs * kPairBytes), o2(in_pairs / 2 * kPairBytes);
    for (size_t k = 1; k < l.counts.size(); k++) {
        if (opt.cancel && opt.cancel->load()) return -ECANCELED;
        uint64_t src = 0; /* pairs consumed from level k-1 */
        while (src < l.counts[k - 1]) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(in_pairs, l.counts[k - 1] - src));
            int64_t got = pread_full(ops, fd, in.data(), n * kPairBytes, l.offsets[k - 1] + src * kPairBytes);
            if (got < 0) return static_cast<int>(got);
            if (static_cast<size_t>(got) != n * kPairBytes) return -EIO;
            const size_t no = (n + 1) / 2;
            for (size_t i = 0; i < no; i++) {
                PeakPair a = load_pair(&in[2 * i * kPairBytes]);
                if (2 * i + 1 < n) {
                    PeakPair b = load_pair(&in[(2 * i + 1) * kPairBytes]);
                    a.min = std::min(a.min, b.min);
                    a.max = std::max(a.max, b.max);
                }
                store_pair(&o2[i * kPairBytes], a.min, a.max);
            }
            r = pwrite_all(ops, fd, o2.data(), no * kPairBytes, l.offsets[k] + (src / 2) * kPairBytes);
            if (r < 0) return r;
            src += n;
        }
    }
    return 0;
}

/* Builds the cache into a temporary file and renames it into place. 0 or -errno; -ECANCELED on cancel. */
int build(FileOps &ops, const std::string &take_path, int take_fd, const Meta &m, const Layout &l,
          const PeakBuildOptions &opt) {
    const std::string dir = dir_of(take_path), cache = PeakPyramid::cache_path(take_path);
    const std::string tmp = tmp_name(dir, "pk");
    int fd = ops.open(tmp, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return fd;
    FdGuard g(ops, fd);
    int r = fill(ops, fd, take_fd, m, l, opt);
    if (r == 0) r = ops.fdatasync(fd);
    const int c = ops.close(g.release());
    if (r == 0) r = c;
    if (r == 0) r = ops.rename(tmp, cache);
    if (r != 0) ops.unlink(tmp);
    return r;
}

}  // namespace

PeakPyramid::~PeakPyramid() {
    if (cache_fd_ >= 0) ops_->close(cache_fd_);
    if (take_fd_ >= 0) ops_->close(take_fd_);
}

std::unique_ptr<PeakPyramid> PeakPyramid::open_or_build(FileOps &ops, const std::string &take_path, std::string *error,
                                                        const PeakBuildOptions &opt) {
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return std::unique_ptr<PeakPyramid>();
    };
    HeaderInfo h = parse_header(ops, take_path);
    if (!h.ok) return fail("unreadable take: " + h.error);
    StatInfo st;
    int r = ops.stat(take_path, st);
    if (r < 0) return fail("cannot stat the take (" + std::to_string(-r) + ")");
    uint64_t avail = h.actual_data;
    if (!h.suspicious() && h.declared_data < avail) avail = h.declared_data;

    Meta m;
    m.take_size = st.size;
    m.take_mtime = st.mtime;
    m.frames = avail / h.block_align;
    m.data_offset = h.data_offset;
    m.rate = h.fmt.rate;
    m.channels = h.fmt.channels;
    m.bits = h.fmt.bits;
    m.block_align = h.block_align;
    const Layout l = compute_layout(m.frames);

    int take_fd = ops.open(take_path, O_RDONLY, 0);
    if (take_fd < 0) return fail("cannot open the take (" + std::to_string(-take_fd) + ")");
    FdGuard tg(ops, take_fd);

    const std::string cache = cache_path(take_path);
    bool rebuilt = false;
    int cfd = ops.open(cache, O_RDONLY, 0);
    if (cfd >= 0 && !cache_valid(ops, cfd, m, l)) {
        ops.close(cfd);
        cfd = -1;
    }
    if (cfd < 0) {
        r = build(ops, take_path, take_fd, m, l, opt);
        if (r == -ECANCELED) return fail("cancelled");
        if (r < 0) return fail("cannot write the peak cache (" + std::to_string(-r) + ")");
        rebuilt = true;
        cfd = ops.open(cache, O_RDONLY, 0);
        if (cfd < 0) return fail("cannot open the peak cache (" + std::to_string(-cfd) + ")");
    }
    std::unique_ptr<PeakPyramid> p(new PeakPyramid());
    p->ops_ = &ops;
    p->cache_fd_ = cfd;
    p->take_fd_ = tg.release();
    p->frames_ = m.frames;
    p->data_offset_ = m.data_offset;
    p->rate_ = m.rate;
    p->block_align_ = m.block_align;
    p->channels_ = m.channels;
    p->bits_ = m.bits;
    p->counts_ = l.counts;
    p->offsets_ = l.offsets;
    p->rebuilt_ = rebuilt;
    return p;
}

size_t PeakPyramid::level_for(uint64_t fpb) const {
    size_t k = 0;
    while (k + 1 < counts_.size() && k + 1 < 56 && (static_cast<uint64_t>(kPeakBlock) << (k + 1)) <= fpb) k++;
    return k;
}

std::vector<PeakPair> PeakPyramid::window(uint64_t start, uint64_t fpb, size_t bars) {
    std::vector<PeakPair> res(bars);
    if (bars == 0 || frames_ == 0 || start >= frames_) return res;
    if (fpb == 0) fpb = 1;
    typedef unsigned __int128 u128;

    if (fpb < kPeakBlock) { /* exact, from the take */
        const uint64_t want = std::min<uint64_t>(frames_ - start, static_cast<uint64_t>(bars) * fpb);
        std::vector<unsigned char> buf(static_cast<size_t>(want) * block_align_);
        int64_t got = pread_full(*ops_, take_fd_, buf.data(), buf.size(), data_offset_ + start * block_align_);
        if (got < 0) return res;
        const uint64_t have = static_cast<uint64_t>(got) / block_align_;
        for (size_t i = 0; i < bars; i++) {
            const uint64_t f0 = i * fpb;
            if (f0 >= have) break;
            const uint64_t n = std::min<uint64_t>(fpb, have - f0);
            int mn, mx;
            scan(&buf[static_cast<size_t>(f0) * block_align_], static_cast<size_t>(n) * channels_, bits_ / 8u, mn, mx);
            res[i].min = static_cast<int16_t>(mn);
            res[i].max = static_cast<int16_t>(mx);
        }
        return res;
    }

    const size_t k = level_for(fpb);
    const uint64_t bs = static_cast<uint64_t>(kPeakBlock) << k;
    const uint64_t nblk = counts_[k];
    struct Span {
        uint64_t b0, b1;
    };
    std::vector<Span> spans(bars, Span{0, 0});
    uint64_t lo = UINT64_MAX, hi = 0;
    for (size_t i = 0; i < bars; i++) {
        const u128 s = static_cast<u128>(start) + static_cast<u128>(i) * fpb;
        if (s >= frames_) break;
        u128 e = s + fpb;
        if (e > frames_) e = frames_;
        Span sp{static_cast<uint64_t>(s / bs), static_cast<uint64_t>((e + bs - 1) / bs)};
        if (sp.b1 > nblk) sp.b1 = nblk;
        if (sp.b1 <= sp.b0) continue;
        spans[i] = sp;
        lo = std::min(lo, sp.b0);
        hi = std::max(hi, sp.b1);
    }
    if (hi <= lo) return res;
    std::vector<unsigned char> buf(static_cast<size_t>(hi - lo) * kPairBytes);
    int64_t got = pread_full(*ops_, cache_fd_, buf.data(), buf.size(), offsets_[k] + lo * kPairBytes);
    if (got != static_cast<int64_t>(buf.size())) return std::vector<PeakPair>(bars);
    for (size_t i = 0; i < bars; i++) {
        if (spans[i].b1 <= spans[i].b0) continue;
        PeakPair a = load_pair(&buf[static_cast<size_t>(spans[i].b0 - lo) * kPairBytes]);
        for (uint64_t b = spans[i].b0 + 1; b < spans[i].b1; b++) {
            PeakPair c = load_pair(&buf[static_cast<size_t>(b - lo) * kPairBytes]);
            a.min = std::min(a.min, c.min);
            a.max = std::max(a.max, c.max);
        }
        res[i] = a;
    }
    return res;
}

}  // namespace cassette
