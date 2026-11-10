/* SPDX-License-Identifier: MIT */
#include "stream_scan.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <vector>

namespace cassette {
namespace {

struct Tables {
    uint8_t c8[256];
    uint16_t c16[256];
    uint32_t c32[256];
    Tables() {
        for (unsigned i = 0; i < 256; ++i) {
            uint8_t a = static_cast<uint8_t>(i);
            for (int k = 0; k < 8; ++k) a = static_cast<uint8_t>((a & 0x80) ? (a << 1) ^ 0x07 : (a << 1));
            c8[i] = a;
            uint16_t b = static_cast<uint16_t>(i << 8);
            for (int k = 0; k < 8; ++k) b = static_cast<uint16_t>((b & 0x8000) ? (b << 1) ^ 0x8005 : (b << 1));
            c16[i] = b;
            uint32_t c = static_cast<uint32_t>(i) << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            c32[i] = c;
        }
    }
};
const Tables &tables() {
    static const Tables t;
    return t;
}

inline uint16_t crc16_step(uint16_t crc, unsigned char b) {
    return static_cast<uint16_t>((crc << 8) ^ tables().c16[((crc >> 8) ^ b) & 0xFF]);
}

uint32_t le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const unsigned char *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

/* A forward reader over a descriptor with one buffered window (pread through FileOps). */
class Reader {
public:
    Reader(FileOps &ops, int fd, uint64_t size, size_t window) : ops_(ops), fd_(fd), size_(size), window_(window) {}
    uint64_t size() const { return size_; }
    /* [pos, pos + n) as a pointer valid until the next call; null when it passes the end or the read fails. */
    const unsigned char *get(uint64_t pos, size_t n) {
        if (pos > size_ || n > size_ - pos) return nullptr;
        if (pos >= base_ && pos + n <= base_ + len_) return buf_.data() + (pos - base_);
        if (!fill(pos, n)) return nullptr;
        return buf_.data() + (pos - base_);
    }
    /* The byte at pos, -1 past the end. */
    int byte(uint64_t pos) {
        if (pos >= base_ && pos < base_ + len_) return buf_[static_cast<size_t>(pos - base_)];
        const unsigned char *p = get(pos, 1);
        return p ? *p : -1;
    }

private:
    bool fill(uint64_t pos, size_t n) {
        const size_t len = static_cast<size_t>(std::min<uint64_t>(std::max(n, window_), size_ - pos));
        if (buf_.size() < len) buf_.resize(len);
        size_t got = 0;
        while (got < len) {
            const ssize_t k = ops_.pread(fd_, buf_.data() + got, len - got, pos + got);
            if (k == -EINTR) continue;
            if (k <= 0) break;
            got += static_cast<size_t>(k);
        }
        base_ = pos;
        len_ = got;
        return got >= n;
    }
    FileOps &ops_;
    int fd_;
    uint64_t size_;
    size_t window_;
    std::vector<unsigned char> buf_;
    uint64_t base_ = 0;
    size_t len_ = 0;
};

struct OpenFile {
    FileOps &ops;
    int fd = -1;
    uint64_t size = 0;
    OpenFile(FileOps &o, const std::string &path, int flags) : ops(o) {
        fd = ops.open(path, flags, 0);
        if (fd < 0) return;
        StatInfo st;
        const int r = ops.fstat(fd, st);
        if (r < 0) {
            ops.close(fd);
            fd = r;
            return;
        }
        size = st.size;
    }
    ~OpenFile() {
        if (fd >= 0) ops.close(fd);
    }
    OpenFile(const OpenFile &) = delete;
    OpenFile &operator=(const OpenFile &) = delete;
};

int pwrite_all(FileOps &ops, int fd, const unsigned char *p, size_t n, uint64_t off) {
    while (n > 0) {
        const ssize_t w = ops.pwrite(fd, p, n, off);
        if (w == -EINTR) continue;
        if (w < 0) return static_cast<int>(w);
        if (w == 0) return -EIO;
        p += w;
        n -= static_cast<size_t>(w);
        off += static_cast<uint64_t>(w);
    }
    return 0;
}

/* ---------------------------------------------------------------- FLAC */

struct FlacMeta {
    bool ok = false;
    std::string error;
    uint32_t min_block = 0, max_block = 0, min_frame = 0, max_frame = 0, rate = 0;
    unsigned channels = 0, bits = 0;
    uint64_t total = 0;
    bool md5_set = false;
    uint64_t si_offset = 0; /* the STREAMINFO body */
    uint64_t audio_offset = 0;
};

FlacMeta read_flac_meta(Reader &r) {
    FlacMeta m;
    const unsigned char *p = r.get(0, 4);
    if (!p || std::memcmp(p, "fLaC", 4) != 0) {
        m.error = "not a FLAC file";
        return m;
    }
    uint64_t pos = 4;
    bool last = false, first = true;
    while (!last) {
        p = r.get(pos, 4);
        if (!p) {
            m.error = "the FLAC header is cut";
            return m;
        }
        last = (p[0] & 0x80) != 0;
        const unsigned type = p[0] & 0x7F;
        const uint32_t len = (static_cast<uint32_t>(p[1]) << 16) | (p[2] << 8) | p[3];
        if (type == 127) {
            m.error = "a broken FLAC header";
            return m;
        }
        if (first) {
            if (type != 0 || len != 34) {
                m.error = "no STREAMINFO";
                return m;
            }
            const unsigned char *b = r.get(pos + 4, 34);
            if (!b) {
                m.error = "the FLAC header is cut";
                return m;
            }
            m.si_offset = pos + 4;
            m.min_block = (b[0] << 8) | b[1];
            m.max_block = (b[2] << 8) | b[3];
            m.min_frame = (static_cast<uint32_t>(b[4]) << 16) | (b[5] << 8) | b[6];
            m.max_frame = (static_cast<uint32_t>(b[7]) << 16) | (b[8] << 8) | b[9];
            m.rate = (static_cast<uint32_t>(b[10]) << 12) | (b[11] << 4) | (b[12] >> 4);
            m.channels = ((b[12] >> 1) & 7) + 1;
            m.bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            m.total = (static_cast<uint64_t>(b[13] & 0x0F) << 32) | (static_cast<uint64_t>(b[14]) << 24) |
                      (static_cast<uint64_t>(b[15]) << 16) | (static_cast<uint64_t>(b[16]) << 8) | b[17];
            m.md5_set = !std::all_of(b + 18, b + 34, [](unsigned char c) { return c == 0; });
            first = false;
        }
        pos += 4 + static_cast<uint64_t>(len);
        if (pos > r.size()) {
            m.error = "the FLAC header is cut";
            return m;
        }
    }
    if (m.rate == 0 || m.bits < 4) {
        m.error = "a broken STREAMINFO";
        return m;
    }
    m.audio_offset = pos;
    m.ok = true;
    return m;
}

struct FrameHdr {
    bool variable = false;
    uint64_t number = 0; /* frame number (fixed block size) or first sample number (variable) */
    uint32_t block = 0;
    size_t len = 0;      /* header bytes, CRC-8 included */
    unsigned assignment = 0; /* channel assignment code: 0..7 independent, 8 left/side, 9 side/right, 10 mid/side */
    unsigned bits = 0;       /* sample size of the frame (from the header, or STREAMINFO's) */
};

/* RFC 9639 section 9.1: a frame header at pos that passes its CRC-8 and agrees with STREAMINFO. */
bool parse_frame_header(Reader &r, uint64_t pos, const FlacMeta &m, FrameHdr &h) {
    if (pos >= r.size()) return false;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(16, r.size() - pos));
    if (n < 6) return false;
    const unsigned char *src = r.get(pos, n);
    if (!src) return false;
    unsigned char b[16];
    std::memcpy(b, src, n);
    if (b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) return false;
    h.variable = (b[1] & 1) != 0;
    const unsigned bs = b[2] >> 4, sr = b[2] & 15, ch = b[3] >> 4, ss = (b[3] >> 1) & 7;
    if ((b[3] & 1) || bs == 0 || sr == 15 || ch > 10 || ss == 3) return false;
    size_t i = 4;
    const unsigned c = b[i++];
    int extra = 0;
    uint64_t v = 0;
    if (!(c & 0x80)) v = c, extra = 0;
    else if ((c & 0xE0) == 0xC0) v = c & 0x1F, extra = 1;
    else if ((c & 0xF0) == 0xE0) v = c & 0x0F, extra = 2;
    else if ((c & 0xF8) == 0xF0) v = c & 0x07, extra = 3;
    else if ((c & 0xFC) == 0xF8) v = c & 0x03, extra = 4;
    else if ((c & 0xFE) == 0xFC) v = c & 0x01, extra = 5;
    else if (c == 0xFE) v = 0, extra = 6;
    else return false;
    if (extra == 6 && !h.variable) return false;
    if (i + static_cast<size_t>(extra) > n) return false;
    for (int k = 0; k < extra; ++k) {
        const unsigned cc = b[i++];
        if ((cc & 0xC0) != 0x80) return false;
        v = (v << 6) | (cc & 0x3F);
    }
    h.number = v;
    uint32_t block = 0;
    if (bs == 1) block = 192;
    else if (bs <= 5) block = 144u << bs;
    else if (bs == 6) {
        if (i + 1 > n) return false;
        block = b[i++] + 1u;
    } else if (bs == 7) {
        if (i + 2 > n) return false;
        block = ((static_cast<uint32_t>(b[i]) << 8) | b[i + 1]) + 1u;
        i += 2;
    } else {
        block = 1u << bs;
    }
    uint32_t rate = 0;
    static const uint32_t kRates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    if (sr == 12) {
        if (i + 1 > n) return false;
        rate = b[i++] * 1000u;
    } else if (sr == 13 || sr == 14) {
        if (i + 2 > n) return false;
        rate = (static_cast<uint32_t>(b[i]) << 8) | b[i + 1];
        if (sr == 14) rate *= 10;
        i += 2;
    } else {
        rate = kRates[sr];
    }
    if (i + 1 > n) return false;
    if (flac_crc8(b, i) != b[i]) return false;
    h.len = i + 1;
    h.block = block;
    h.assignment = ch;
    const unsigned channels = ch < 8 ? ch + 1 : 2;
    if (channels != m.channels) return false;
    static const unsigned kBits[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    if (ss && kBits[ss] != m.bits) return false;
    h.bits = ss ? kBits[ss] : m.bits;
    if (sr && rate != m.rate) return false;
    if (m.max_block && block > m.max_block) return false;
    return true;
}

bool follows(const FrameHdr &a, const FrameHdr &b) {
    if (a.variable != b.variable) return false;
    return a.variable ? b.number == a.number + a.block : b.number == a.number + 1;
}
bool later(const FrameHdr &a, const FrameHdr &b) {
    if (a.variable != b.variable) return false;
    return a.variable ? b.number >= a.number + a.block : b.number > a.number;
}

/* A big-endian bit reader over the file, from a bit position up to the end of the file. */
struct Bits {
    Reader &r;
    uint64_t bp, end;
    bool bit(unsigned &v) {
        if (bp >= end) return false;
        const int by = r.byte(bp >> 3);
        if (by < 0) return false;
        v = (static_cast<unsigned>(by) >> (7 - (bp & 7))) & 1u;
        ++bp;
        return true;
    }
    bool read(unsigned n, uint64_t &v) {
        v = 0;
        for (unsigned i = 0; i < n; ++i) {
            unsigned b;
            if (!bit(b)) return false;
            v = (v << 1) | b;
        }
        return true;
    }
    bool skip(uint64_t n) {
        if (bp > end || end - bp < n) return false;
        bp += n;
        return true;
    }
    /* zeros up to and including the next one bit */
    bool unary(uint64_t &zeros) {
        zeros = 0;
        for (;;) {
            if ((bp & 7) == 0 && bp + 8 <= end && r.byte(bp >> 3) == 0) { /* a whole zero byte */
                bp += 8;
                zeros += 8;
                continue;
            }
            unsigned b;
            if (!bit(b)) return false;
            if (b) return true;
            ++zeros;
        }
    }
};

bool skip_residual(Bits &b, uint32_t block, unsigned order) {
    uint64_t method, porder;
    if (!b.read(2, method) || method > 1 || !b.read(4, porder)) return false;
    const unsigned pbits = method == 0 ? 4 : 5;
    const uint64_t escape = method == 0 ? 15 : 31;
    const uint32_t parts = 1u << porder;
    if (block % parts != 0 || (block >> porder) < order) return false;
    for (uint32_t i = 0; i < parts; ++i) {
        const uint64_t n = (block >> porder) - (i == 0 ? order : 0);
        uint64_t param;
        if (!b.read(pbits, param)) return false;
        if (param == escape) {
            uint64_t raw;
            if (!b.read(5, raw) || !b.skip(n * raw)) return false;
            continue;
        }
        for (uint64_t s = 0; s < n; ++s) {
            uint64_t q;
            if (!b.unary(q) || !b.skip(param)) return false;
        }
    }
    return true;
}

/* The exact length of the frame at p by walking its subframes (RFC 9639 section 9.2: CONSTANT, VERBATIM, FIXED, LPC
 * with Rice residuals): the byte after its CRC-16, or 0 when the frame does not parse or runs past the end of the file.
 * Used for the last frame of a file, where no next header tells where it ends. */
uint64_t frame_length_end(Reader &r, uint64_t p, const FrameHdr &h) {
    Bits b{r, (p + h.len) * 8, r.size() * 8};
    const unsigned channels = h.assignment < 8 ? h.assignment + 1 : 2;
    for (unsigned c = 0; c < channels; ++c) {
        uint64_t bps = h.bits;
        if ((h.assignment == 8 && c == 1) || (h.assignment == 9 && c == 0) || (h.assignment == 10 && c == 1))
            ++bps; /* the side channel has one bit more */
        uint64_t pad, type, wasted;
        if (!b.read(1, pad) || pad != 0 || !b.read(6, type) || !b.read(1, wasted)) return 0;
        if (wasted) {
            uint64_t z;
            if (!b.unary(z) || z + 1 >= bps) return 0;
            bps -= z + 1;
        }
        if (type == 0) {
            if (!b.skip(bps)) return 0;
        } else if (type == 1) {
            if (!b.skip(bps * h.block)) return 0;
        } else if ((type & 0x38) == 0x08) {
            const unsigned order = static_cast<unsigned>(type & 7);
            if (order > 4 || order > h.block || !b.skip(bps * order) || !skip_residual(b, h.block, order)) return 0;
        } else if (type & 0x20) {
            const unsigned order = static_cast<unsigned>(type & 0x1F) + 1;
            uint64_t prec, shift;
            if (order > h.block || !b.skip(bps * order) || !b.read(4, prec) || prec == 15 || !b.read(5, shift) ||
                !b.skip((prec + 1) * order) || !skip_residual(b, h.block, order))
                return 0;
        } else {
            return 0; /* reserved */
        }
    }
    const uint64_t end = (b.bp + 7) / 8 + 2;
    return end <= r.size() ? end : 0;
}

/* The end of the frame at p, 0 when the frame is not whole and valid. A middle frame: the first e within max_bytes
 * where the CRC-16 of [p, e - 2) equals the two bytes before e and the frame that follows starts at e (header valid,
 * the next number). The last frame (no such e): its exact length from its subframes, then the same CRC-16 check, so a
 * cut frame or whatever follows the last frame (zeros, a cut header, garbage) never counts. */
uint64_t find_frame_end(Reader &r, uint64_t p, const FrameHdr &h, const FlacMeta &m, uint64_t max_bytes) {
    const uint64_t size = r.size();
    const uint64_t limit = std::min(size, p + max_bytes);
    uint64_t k = p + h.len + 1; /* the first byte the CRC-16 could start at: a subframe has at least one byte */
    const unsigned char *hp = k + 2 <= limit ? r.get(p, h.len + 1) : nullptr;
    if (hp) {
        uint16_t crc = flac_crc16(hp, h.len + 1);
        for (; k + 2 < limit; ++k) {
            const int c0 = r.byte(k), c1 = r.byte(k + 1);
            if (c0 < 0 || c1 < 0) break;
            if (crc == static_cast<uint16_t>((c0 << 8) | c1) && r.byte(k + 2) == 0xFF) {
                FrameHdr nh;
                if (parse_frame_header(r, k + 2, m, nh) && follows(h, nh)) return k + 2;
            }
            crc = crc16_step(crc, static_cast<unsigned char>(c0));
        }
    }
    const uint64_t e = frame_length_end(r, p, h);
    if (e < p + h.len + 3) return 0;
    const uint64_t n = e - 2 - p;
    uint16_t crc = 0;
    for (uint64_t q = p; q < p + n; ++q) {
        const int v = r.byte(q);
        if (v < 0) return 0;
        crc = crc16_step(crc, static_cast<unsigned char>(v));
    }
    const int c0 = r.byte(e - 2), c1 = r.byte(e - 1);
    if (c0 < 0 || c1 < 0 || crc != static_cast<uint16_t>((c0 << 8) | c1)) return 0;
    return e;
}

/* ---------------------------------------------------------------- Ogg */

struct Page {
    uint64_t off = 0;
    size_t len = 0;
    unsigned type = 0;
    uint64_t granule = 0;
    uint32_t serial = 0;
    bool packet_ends = false; /* a lacing value below 255: at least one packet completes on this page */
};

/* A whole page at pos whose CRC matches. body (optional) receives its body. */
bool read_page(Reader &r, uint64_t pos, Page &pg, std::vector<unsigned char> *body = nullptr) {
    const unsigned char *h = r.get(pos, 27);
    if (!h || std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    const unsigned nseg = h[26];
    const unsigned char *lac = r.get(pos, 27 + nseg);
    if (!lac) return false;
    size_t blen = 0;
    bool ends = false;
    for (unsigned i = 0; i < nseg; ++i) {
        blen += lac[27 + i];
        if (lac[27 + i] < 255) ends = true;
    }
    const size_t len = 27 + nseg + blen;
    const unsigned char *p = r.get(pos, len);
    if (!p) return false;
    static const unsigned char kZero[4] = {0, 0, 0, 0};
    uint32_t crc = ogg_crc32(p, 22);
    crc = ogg_crc32(kZero, 4, crc);
    crc = ogg_crc32(p + 26, len - 26, crc);
    if (crc != le32(p + 22)) return false;
    pg.off = pos;
    pg.len = len;
    pg.type = p[5];
    pg.granule = le64(p + 6);
    pg.serial = le32(p + 14);
    pg.packet_ends = ends;
    if (body) body->assign(p + 27 + nseg, p + len);
    return true;
}

struct OpusHead {
    bool ok = false;
    std::string error;
    uint32_t serial = 0, preskip = 0, rate = 48000;
    unsigned channels = 0;
    uint64_t end = 0; /* the end of the first page */
};

OpusHead read_opus_head(Reader &r) {
    OpusHead o;
    Page pg;
    std::vector<unsigned char> body;
    const unsigned char *m = r.get(0, 4);
    if (!m || std::memcmp(m, "OggS", 4) != 0) {
        o.error = "not an Ogg file";
        return o;
    }
    if (!read_page(r, 0, pg, &body)) {
        o.error = "the Opus header is cut";
        return o;
    }
    if (body.size() < 19 || std::memcmp(body.data(), "OpusHead", 8) != 0 || (body[8] & 0xF0) != 0 || body[9] == 0) {
        o.error = "not an Ogg Opus file";
        return o;
    }
    o.serial = pg.serial;
    o.channels = body[9];
    o.preskip = body[10] | (body[11] << 8);
    const uint32_t rate = le32(body.data() + 12);
    o.rate = rate ? rate : 48000;
    o.end = pg.len;
    o.ok = true;
    return o;
}

uint64_t opus_frames(uint64_t granule, uint32_t preskip, uint32_t rate) {
    if (granule == ~0ull || granule <= preskip) return 0;
    return (granule - preskip) * rate / 48000u;
}

std::string lower_ext(const std::string &name) {
    const size_t slash = name.rfind('/');
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = name.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

}  // namespace

/* ---------------------------------------------------------------- checksums */

uint8_t flac_crc8(const unsigned char *p, size_t n) {
    uint8_t crc = 0;
    const Tables &t = tables();
    for (size_t i = 0; i < n; ++i) crc = t.c8[crc ^ p[i]];
    return crc;
}

uint16_t flac_crc16(const unsigned char *p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; ++i) crc = crc16_step(crc, p[i]);
    return crc;
}

uint32_t ogg_crc32(const unsigned char *p, size_t n, uint32_t crc) {
    const Tables &t = tables();
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ t.c32[((crc >> 24) & 0xFF) ^ p[i]];
    return crc;
}

/* ---------------------------------------------------------------- containers */

Container container_of_name(const std::string &name) {
    const std::string e = lower_ext(name);
    if (e == "wav") return Container::Wav;
    if (e == "flac") return Container::Flac;
    if (e == "opus") return Container::OggOpus;
    return Container::Unknown;
}

Container container_of(FileOps &ops, const std::string &path) {
    const int fd = ops.open(path, O_RDONLY, 0);
    if (fd >= 0) {
        unsigned char b[4];
        const ssize_t k = ops.pread(fd, b, 4, 0);
        ops.close(fd);
        if (k == 4) {
            if (std::memcmp(b, "RIFF", 4) == 0 || std::memcmp(b, "RF64", 4) == 0) return Container::Wav;
            if (std::memcmp(b, "fLaC", 4) == 0) return Container::Flac;
            if (std::memcmp(b, "OggS", 4) == 0) return Container::OggOpus;
        }
    }
    return container_of_name(path);
}

StreamInfo read_stream_info(FileOps &ops, const std::string &path) {
    StreamInfo i;
    OpenFile f(ops, path, O_RDONLY);
    if (f.fd < 0) {
        i.error = "cannot open (" + std::string(std::strerror(-f.fd)) + ")";
        return i;
    }
    i.file_size = f.size;
    Reader r(ops, f.fd, f.size, 64 * 1024);
    const unsigned char *m = r.get(0, 4);
    if (!m) {
        i.error = "too short";
        return i;
    }
    if (std::memcmp(m, "fLaC", 4) == 0) {
        i.kind = Container::Flac;
        const FlacMeta fm = read_flac_meta(r);
        if (!fm.ok) {
            i.error = fm.error;
            return i;
        }
        i.fmt.rate = fm.rate;
        i.fmt.channels = static_cast<uint16_t>(fm.channels);
        i.fmt.bits = static_cast<uint16_t>(fm.bits);
        i.frames = fm.total;
        i.frames_known = fm.total != 0;
        i.ok = true;
        return i;
    }
    if (std::memcmp(m, "OggS", 4) == 0) {
        i.kind = Container::OggOpus;
        const OpusHead h = read_opus_head(r);
        if (!h.ok) {
            i.error = h.error;
            return i;
        }
        i.fmt.rate = h.rate;
        i.fmt.channels = static_cast<uint16_t>(h.channels);
        i.fmt.bits = 16;
        i.ok = true;
        /* the duration: the granule position of the last whole page in the last 128 KB */
        const uint64_t from = f.size > 131072 ? f.size - 131072 : 0;
        if (f.size >= 27 && r.get(from, static_cast<size_t>(f.size - from))) {
            for (uint64_t q = f.size - 27 + 1; q-- > from;) {
                if (r.byte(q) != 'O') continue;
                Page pg;
                if (!read_page(r, q, pg) || pg.serial != h.serial || pg.granule == ~0ull || q == 0) continue;
                i.frames = opus_frames(pg.granule, h.preskip, h.rate);
                i.frames_known = true;
                break;
            }
        }
        return i;
    }
    i.error = "not a FLAC or Ogg Opus file";
    return i;
}

/* ---------------------------------------------------------------- scans */

StreamScan scan_flac(FileOps &ops, const std::string &path) {
    StreamScan s;
    s.kind = Container::Flac;
    OpenFile f(ops, path, O_RDONLY);
    if (f.fd < 0) {
        s.error = "cannot open (" + std::string(std::strerror(-f.fd)) + ")";
        return s;
    }
    s.file_size = f.size;
    Reader r(ops, f.fd, f.size, 1u << 20);
    const FlacMeta m = read_flac_meta(r);
    if (!m.ok) {
        s.error = m.error;
        return s;
    }
    s.ok = true;
    s.fmt.rate = m.rate;
    s.fmt.channels = static_cast<uint16_t>(m.channels);
    s.fmt.bits = static_cast<uint16_t>(m.bits);
    s.stored_total = m.total;
    s.md5_set = m.md5_set;
    s.streaminfo_offset = m.si_offset;
    s.audio_offset = s.valid_end = m.audio_offset;
    const uint64_t max_bytes = m.max_frame > 0 ? static_cast<uint64_t>(m.max_frame) + 16
                                               : static_cast<uint64_t>(m.max_block ? m.max_block : 65535) * m.channels * 5 + 4096;
    uint64_t p = m.audio_offset;
    bool have = false;
    FrameHdr last;
    uint32_t first_block = 0;
    while (p < f.size) {
        FrameHdr h;
        if (r.byte(p) == 0xFF && parse_frame_header(r, p, m, h) && (!have || later(last, h))) {
            const uint64_t e = find_frame_end(r, p, h, m, max_bytes);
            if (e) {
                if (p != s.valid_end) s.damaged++;
                if (!have) first_block = h.block;
                have = true;
                last = h;
                s.valid_end = e;
                s.units++;
                p = e;
                continue;
            }
        }
        ++p; /* resync: the next byte that starts a valid frame */
    }
    if (have) {
        if (last.variable) {
            s.frames = last.number + last.block;
        } else {
            const uint64_t nominal = (m.min_block == m.max_block && m.max_block) ? m.max_block : first_block;
            s.frames = last.number * nominal + last.block;
        }
    }
    return s;
}

StreamScan scan_ogg_opus(FileOps &ops, const std::string &path) {
    StreamScan s;
    s.kind = Container::OggOpus;
    OpenFile f(ops, path, O_RDONLY);
    if (f.fd < 0) {
        s.error = "cannot open (" + std::string(std::strerror(-f.fd)) + ")";
        return s;
    }
    s.file_size = f.size;
    Reader r(ops, f.fd, f.size, 1u << 20);
    const OpusHead h = read_opus_head(r);
    if (!h.ok) {
        s.error = h.error;
        return s;
    }
    s.fmt.rate = h.rate;
    s.fmt.channels = static_cast<uint16_t>(h.channels);
    s.fmt.bits = 16;
    s.preskip = h.preskip;
    /* OpusTags: complete on its own page(s) before the audio (RFC 7845, section 3) */
    uint64_t p = h.end;
    uint64_t last_hdr = 0;
    Page pg;
    for (;;) {
        if (!read_page(r, p, pg) || pg.serial != h.serial) {
            s.error = "the Opus header is cut";
            return s;
        }
        last_hdr = p;
        p += pg.len;
        if (pg.packet_ends) break;
    }
    s.ok = true;
    s.audio_offset = s.valid_end = p;
    s.last_page = last_hdr;
    s.eos = (pg.type & 0x04) != 0;
    s.header_only = true;
    s.units = 2;
    uint64_t next = p;
    while (p < f.size && !s.eos) {
        if (r.byte(p) == 'O' && read_page(r, p, pg)) {
            if (pg.serial != h.serial) { /* another logical stream: not ours, step over it */
                p += pg.len;
                continue;
            }
            if (p != next) s.damaged++;
            s.units++;
            p += pg.len;
            next = p;
            if (pg.granule != ~0ull) { /* a page on which a packet ends: a place the stream can stop */
                s.valid_end = p;
                s.last_page = pg.off;
                s.last_granule = pg.granule;
                s.eos = (pg.type & 0x04) != 0;
                s.header_only = false;
            }
            continue;
        }
        ++p;
    }
    if (!s.header_only) s.frames = opus_frames(s.last_granule, s.preskip, h.rate);
    return s;
}

/* ---------------------------------------------------------------- repairs */

StreamFix fix_flac(FileOps &ops, const std::string &path, bool force) {
    StreamFix x;
    const StreamScan s = scan_flac(ops, path);
    if (!s.ok) {
        x.error = s.error;
        return x;
    }
    x.fmt = s.fmt;
    x.frames = s.frames;
    const bool cut = s.valid_end < s.file_size;
    const bool total_wrong = s.stored_total != s.frames;
    /* without a journal only an unfinished file (total 0: libFLAC writes the real total when it finishes) */
    const bool act = force ? (cut || total_wrong) : (s.stored_total == 0 && (cut || s.frames > 0));
    if (!act) {
        x.ok = true;
        return x;
    }
    const int fd = ops.open(path, O_WRONLY, 0);
    if (fd < 0) {
        x.error = "cannot open for repair (" + std::to_string(-fd) + ")";
        return x;
    }
    int rc = 0;
    if (cut) rc = ops.ftruncate(fd, s.valid_end); /* first: a crash here leaves a repairable file */
    if (rc == 0 && total_wrong) {
        if (s.md5_set) { /* the MD5 was for another length: zero means "unknown" (RFC 9639) */
            const unsigned char zero[16] = {};
            rc = pwrite_all(ops, fd, zero, sizeof zero, s.streaminfo_offset + 18);
        }
        if (rc == 0) {
            const uint64_t t = std::min<uint64_t>(s.frames, (1ull << 36) - 1);
            const unsigned char b[5] = {static_cast<unsigned char>((((s.fmt.bits - 1u) & 0x0F) << 4) | ((t >> 32) & 0x0F)),
                                        static_cast<unsigned char>(t >> 24), static_cast<unsigned char>(t >> 16),
                                        static_cast<unsigned char>(t >> 8), static_cast<unsigned char>(t)};
            rc = pwrite_all(ops, fd, b, sizeof b, s.streaminfo_offset + 13);
        }
    }
    if (rc == 0) rc = ops.fdatasync(fd);
    ops.close(fd);
    if (rc < 0) {
        x.error = "write failed (" + std::to_string(-rc) + ")";
        return x;
    }
    x.ok = true;
    x.changed = true;
    x.truncated_bytes = cut ? s.file_size - s.valid_end : 0;
    return x;
}

StreamFix fix_ogg_opus(FileOps &ops, const std::string &path, bool force) {
    StreamFix x;
    const StreamScan s = scan_ogg_opus(ops, path);
    if (!s.ok) {
        x.error = s.error;
        return x;
    }
    x.fmt = s.fmt;
    x.frames = s.frames;
    const bool cut = s.valid_end < s.file_size;
    const bool need_eos = !s.eos && !s.header_only;
    const bool act = force ? (cut || need_eos) : (!s.eos && (cut || !s.header_only));
    if (!act) {
        x.ok = true;
        return x;
    }
    const int fd = ops.open(path, O_RDWR, 0);
    if (fd < 0) {
        x.error = "cannot open for repair (" + std::to_string(-fd) + ")";
        return x;
    }
    int rc = 0;
    if (cut) rc = ops.ftruncate(fd, s.valid_end); /* first, as for WAV */
    if (rc == 0 && need_eos) {
        /* the end-of-stream flag on the last page: the flag and the new CRC in one write */
        std::vector<unsigned char> page(static_cast<size_t>(s.valid_end - s.last_page));
        size_t got = 0;
        while (got < page.size()) {
            const ssize_t k = ops.pread(fd, page.data() + got, page.size() - got, s.last_page + got);
            if (k == -EINTR) continue;
            if (k <= 0) break;
            got += static_cast<size_t>(k);
        }
        if (got != page.size() || page.size() < 27) {
            rc = -EIO;
        } else {
            page[5] |= 0x04;
            std::memset(page.data() + 22, 0, 4);
            const uint32_t crc = ogg_crc32(page.data(), page.size());
            for (int k = 0; k < 4; ++k) page[22 + k] = static_cast<unsigned char>((crc >> (8 * k)) & 0xFF);
            rc = pwrite_all(ops, fd, page.data() + 5, 21, s.last_page + 5);
        }
    }
    if (rc == 0) rc = ops.fdatasync(fd);
    ops.close(fd);
    if (rc < 0) {
        x.error = "write failed (" + std::to_string(-rc) + ")";
        return x;
    }
    x.ok = true;
    x.changed = true;
    x.truncated_bytes = cut ? s.file_size - s.valid_end : 0;
    return x;
}

}  // namespace cassette
