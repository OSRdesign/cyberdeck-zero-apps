/* SPDX-License-Identifier: MIT */
#include "ogg_opus.hpp"

#include "codec_libs.hpp"
#include "stream_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace cassette {
namespace oggopus {
namespace {

constexpr size_t kWindow = 16 * 1024; /* one read: a page of 1 s at 128 kbps, or a bisection probe */
constexpr size_t kMaxKnown = 4096;
constexpr uint64_t kLinearSpan = 64 * 1024; /* below this the seek walks the pages instead of bisecting */

uint32_t le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
uint32_t le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const unsigned char *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
void put_le(std::vector<unsigned char> &b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

/* ---------------------------------------------------------------- packets and headers */

uint32_t packet_samples48(const unsigned char *p, size_t len) {
    if (!p || len < 1) return 0;
    const unsigned toc = p[0], config = toc >> 3;
    uint32_t size;
    if (config < 12) {
        static const uint32_t silk[4] = {480, 960, 1920, 2880};
        size = silk[config & 3];
    } else if (config < 16) {
        size = (config & 1) ? 960 : 480;
    } else {
        static const uint32_t celt[4] = {120, 240, 480, 960};
        size = celt[config & 3];
    }
    uint32_t frames;
    switch (toc & 3) {
    case 0: frames = 1; break;
    case 1:
    case 2: frames = 2; break;
    default:
        if (len < 2) return 0;
        frames = p[1] & 0x3F;
        break;
    }
    const uint32_t n = frames * size;
    return (n == 0 || n > kMaxPacket48) ? 0 : n;
}

bool parse_head(const unsigned char *p, size_t n, Head &h, std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    if (n < 19 || std::memcmp(p, "OpusHead", 8) != 0) return no("not an Ogg Opus file");
    h.version = p[8];
    if ((h.version & 0xF0) != 0) return no("an Opus version Cassette does not read");
    h.channels = p[9];
    if (h.channels == 0) return no("OpusHead says 0 channels");
    h.preskip = le16(p + 10);
    h.input_rate = le32(p + 12);
    h.gain_q8 = static_cast<int16_t>(le16(p + 16));
    h.mapping_family = p[18];
    h.mapping.clear();
    if (h.mapping_family != 0) {
        if (n < 21u + h.channels) return no("OpusHead is cut");
        h.mapping.assign(p + 19, p + 21 + h.channels);
    } else if (h.channels > 2) {
        return no("OpusHead: family 0 with more than 2 channels");
    }
    return true;
}

std::vector<unsigned char> build_head(const Head &h) {
    std::vector<unsigned char> b = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    b.push_back(1);
    b.push_back(static_cast<unsigned char>(h.channels));
    put_le(b, h.preskip, 2);
    put_le(b, h.input_rate, 4);
    put_le(b, static_cast<uint16_t>(h.gain_q8), 2);
    b.push_back(static_cast<unsigned char>(h.mapping_family));
    if (h.mapping_family != 0) b.insert(b.end(), h.mapping.begin(), h.mapping.end());
    return b;
}

bool parse_tags(const unsigned char *p, size_t n, std::string &vendor, Tags &tags) {
    tags.clear();
    vendor.clear();
    if (n < 16 || std::memcmp(p, "OpusTags", 8) != 0) return false;
    size_t q = 8;
    const uint32_t vl = le32(p + q);
    q += 4;
    if (vl > n - q) return false;
    vendor.assign(reinterpret_cast<const char *>(p + q), vl);
    q += vl;
    if (n - q < 4) return false;
    const uint32_t count = le32(p + q);
    q += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (n - q < 4) return false;
        const uint32_t l = le32(p + q);
        q += 4;
        if (l > n - q) return false;
        const std::string s(reinterpret_cast<const char *>(p + q), l);
        q += l;
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        tags.push_back(Tag{s.substr(0, eq), s.substr(eq + 1)});
    }
    return true;
}

std::vector<unsigned char> build_tags(const std::string &vendor, const Tags &tags, size_t padding) {
    std::vector<unsigned char> b = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    put_le(b, vendor.size(), 4);
    b.insert(b.end(), vendor.begin(), vendor.end());
    put_le(b, tags.size(), 4);
    for (const Tag &t : tags) {
        const std::string s = t.name + "=" + t.value;
        put_le(b, s.size(), 4);
        b.insert(b.end(), s.begin(), s.end());
    }
    b.insert(b.end(), padding, 0); /* padding whose first byte has bit 0 clear: may be discarded (RFC 7845 5.2) */
    return b;
}

std::vector<unsigned char> build_page(unsigned flags, uint64_t granule, uint32_t serial, uint32_t seq,
                                      const std::vector<std::vector<unsigned char>> &packets) {
    std::vector<unsigned char> lacing, body;
    for (const std::vector<unsigned char> &p : packets) {
        size_t n = p.size();
        while (n >= 255) {
            lacing.push_back(255);
            n -= 255;
        }
        lacing.push_back(static_cast<unsigned char>(n));
        body.insert(body.end(), p.begin(), p.end());
    }
    std::vector<unsigned char> pg = {'O', 'g', 'g', 'S', 0, static_cast<unsigned char>(flags)};
    put_le(pg, granule, 8);
    put_le(pg, serial, 4);
    put_le(pg, seq, 4);
    put_le(pg, 0, 4);
    pg.push_back(static_cast<unsigned char>(lacing.size()));
    pg.insert(pg.end(), lacing.begin(), lacing.end());
    pg.insert(pg.end(), body.begin(), body.end());
    const uint32_t crc = ogg_crc32(pg.data(), pg.size());
    for (int k = 0; k < 4; ++k) pg[22 + static_cast<size_t>(k)] = static_cast<unsigned char>((crc >> (8 * k)) & 0xFF);
    return pg;
}

uint32_t decode_rate(uint32_t input_rate) {
    switch (input_rate) {
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 48000: return input_rate;
    default: return 48000;
    }
}

/* ---------------------------------------------------------------- PageReader */

const unsigned char *PageReader::get(uint64_t pos, size_t n) {
    if (pos > size_ || n > size_ - pos) return nullptr;
    if (pos >= base_ && pos + n <= base_ + len_) return buf_.data() + (pos - base_);
    const size_t want = static_cast<size_t>(std::min<uint64_t>(std::max(n, kWindow), size_ - pos));
    if (buf_.size() < want) buf_.resize(want);
    size_t got = 0;
    while (got < want) {
        const ssize_t k = ops_.pread(fd_, buf_.data() + got, want - got, pos + got);
        if (k == -EINTR) continue;
        if (k < 0) {
            if (!err_) err_ = static_cast<int>(k);
            break;
        }
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    bytes_read_ += got;
    base_ = pos;
    len_ = got;
    return got >= n ? buf_.data() : nullptr;
}

bool PageReader::read_at(uint64_t off, Page &pg, bool want_body) {
    const unsigned char *h = get(off, 27);
    if (!h || std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    const unsigned nseg = h[26];
    const unsigned char *lac = get(off, 27 + nseg);
    if (!lac) return false;
    size_t blen = 0;
    for (unsigned i = 0; i < nseg; ++i) blen += lac[27 + i];
    const size_t len = 27 + nseg + blen;
    const unsigned char *p = get(off, len);
    if (!p) return false;
    static const unsigned char kZero[4] = {0, 0, 0, 0};
    uint32_t crc = ogg_crc32(p, 22);
    crc = ogg_crc32(kZero, 4, crc);
    crc = ogg_crc32(p + 26, len - 26, crc);
    if (crc != le32(p + 22)) return false;
    pg.offset = off;
    pg.header_len = 27 + nseg;
    pg.body_len = blen;
    pg.flags = p[5];
    pg.granule = le64(p + 6);
    pg.serial = le32(p + 14);
    pg.seq = le32(p + 18);
    pg.lacing.assign(p + 27, p + 27 + nseg);
    if (want_body) pg.body.assign(p + 27 + nseg, p + len);
    else pg.body.clear();
    return true;
}

bool PageReader::find_next(uint64_t from, uint64_t limit, Page &pg, bool want_body) {
    limit = std::min(limit, size_);
    uint64_t pos = from;
    while (pos < limit && pos + 27 <= size_) {
        if (!get(pos, 4)) return false;
        /* search the buffered window [pos, end - 3) for the capture pattern */
        const uint64_t wend = base_ + len_;
        const uint64_t qlimit = std::min<uint64_t>(limit, wend - 3);
        uint64_t q = pos;
        bool hit = false;
        while (q < qlimit) {
            const unsigned char *c = buf_.data() + (q - base_);
            const void *m = std::memchr(c, 'O', static_cast<size_t>(qlimit - q));
            if (!m) {
                q = qlimit;
                break;
            }
            q += static_cast<uint64_t>(static_cast<const unsigned char *>(m) - c);
            if (std::memcmp(buf_.data() + (q - base_), "OggS", 4) == 0) {
                hit = true;
                break;
            }
            ++q;
        }
        if (hit) {
            if (read_at(q, pg, want_body)) return true;
            pos = q + 1;
            continue;
        }
        if (q >= limit) return false;
        pos = std::max(q, pos + 1);
    }
    return false;
}

/* ---------------------------------------------------------------- OggOpusFile */

uint64_t OggOpusFile::length48() const {
    if (!has_end_) return 0;
    const int64_t n = end48_ - start48_ - static_cast<int64_t>(head_.preskip);
    return n > 0 ? static_cast<uint64_t>(n) : 0;
}

OggOpusFile::Stats OggOpusFile::stats() const {
    Stats s = st_;
    if (rd_) s.bytes = rd_->bytes_read();
    return s;
}

void OggOpusFile::close() {
    if (fd_ >= 0) ops_.close(fd_);
    fd_ = -1;
    rd_.reset();
    ready_.clear();
    partial_.clear();
    known_.clear();
    has_end_ = eos_ = false;
}

void OggOpusFile::note(const Page &pg) {
    if (pg.granule == kNoGranule || pg.serial != serial_) return;
    if (known_.size() < kMaxKnown) known_[static_cast<int64_t>(pg.granule)] = pg.offset;
}

int OggOpusFile::open(const std::string &path, std::string *why) {
    close();
    auto bad = [&](const std::string &m) {
        if (why) *why = m;
        close();
        return kCodecBadStream;
    };
    const int fd = ops_.open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    fd_ = fd;
    StatInfo st;
    const int r = ops_.fstat(fd, st);
    if (r < 0) {
        close();
        return r;
    }
    size_ = st.size;
    rd_.reset(new PageReader(ops_, fd_, size_));
    Page pg;
    if (!rd_->read_at(0, pg, true)) {
        const int e = rd_->error();
        if (e) {
            close();
            return e;
        }
        return bad("not an Ogg file");
    }
    std::string w;
    if (!(pg.flags & 0x02) || pg.lacing.empty() || pg.lacing.back() == 255 || !parse_head(pg.body.data(), pg.body.size(), head_, &w))
        return bad(w.empty() ? "not an Ogg Opus file" : w);
    serial_ = pg.serial;
    /* OpusTags: one packet on its own page(s), ending a page (RFC 7845 section 3) */
    uint64_t off = pg.end();
    std::vector<unsigned char> tags;
    for (;;) {
        if (!rd_->read_at(off, pg, true)) {
            const int e = rd_->error();
            if (e) {
                close();
                return e;
            }
            return bad("the Opus header is cut");
        }
        off = pg.end();
        if (pg.serial != serial_) continue;
        tags.insert(tags.end(), pg.body.begin(), pg.body.end());
        if (!pg.lacing.empty() && pg.lacing.back() < 255) break;
        if (pg.lacing.empty()) return bad("the Opus header is broken");
    }
    if (!parse_tags(tags.data(), tags.size(), vendor_, tags_)) {
        vendor_.clear();
        tags_.clear(); /* unreadable comments do not stop playback */
    }
    audio_off_ = off;
    /* the last granule: the last page of our stream that has one, searched backwards from the end of the file */
    has_end_ = false;
    uint64_t hi = size_;
    while (hi > audio_off_ && !has_end_) {
        const uint64_t lo = hi > audio_off_ + kWindow ? hi - kWindow : audio_off_;
        std::vector<uint64_t> cands;
        uint64_t q = lo;
        while (rd_->find_next(q, hi, pg, false)) {
            cands.push_back(pg.offset);
            q = pg.end();
        }
        for (size_t i = cands.size(); i-- > 0;) {
            if (!rd_->read_at(cands[i], pg, false) || pg.serial != serial_ || pg.granule == kNoGranule) continue;
            end48_ = static_cast<int64_t>(pg.granule);
            eos_ = (pg.flags & 0x04) != 0;
            has_end_ = true;
            note(pg);
            break;
        }
        if (rd_->error()) {
            const int e = rd_->error();
            close();
            return e;
        }
        if (lo == audio_off_) break;
        hi = lo + 27 < hi ? lo + 27 : lo; /* overlap: a page starting just before `lo` is found next round */
        if (hi <= audio_off_) break;
    }
    /* the start: the first packet's position, computed back from the first page's granule */
    start48_ = 0;
    rewind();
    if (has_end_) {
        Packet p;
        const int n = next(p);
        if (n < 0) {
            close();
            return n;
        }
        if (n == 1) start48_ = p.start48;
        rewind();
    }
    return 0;
}

int OggOpusFile::rewind() {
    ready_.clear();
    ready_i_ = 0;
    partial_.clear();
    partial_ok_ = true;
    next_off_ = audio_off_;
    at_end_ = !has_end_;
    have_last_end_ = false;
    return 0;
}

bool OggOpusFile::load_page(uint64_t off, Page &pg) {
    for (;;) {
        if (rd_->read_at(off, pg, true)) {
            st_.pages++;
            if (pg.serial != serial_) { /* another logical stream: step over it */
                off = pg.end();
                continue;
            }
            note(pg);
            return true;
        }
        if (rd_->error()) return false;
        /* damage: resync on the next valid page; the packet being collected is lost */
        if (!rd_->find_next(off + 1, size_, pg, false)) return false;
        partial_.clear();
        partial_ok_ = false;
        have_last_end_ = false;
        off = pg.offset;
    }
}

int OggOpusFile::start_at_page(const Page &best, bool keep_tail_only) {
    ready_.clear();
    ready_i_ = 0;
    partial_.clear();
    partial_ok_ = false;
    Page pg;
    if (!rd_->read_at(best.offset, pg, true)) return rd_->error() ? rd_->error() : kCodecBadStream;
    st_.pages++;
    next_off_ = pg.end();
    at_end_ = (pg.flags & 0x04) != 0;
    last_end48_ = static_cast<int64_t>(pg.granule);
    have_last_end_ = pg.granule != kNoGranule;
    if (keep_tail_only) {
        /* the packets completing on this page end at or before the target: only a packet that starts on it and
         * continues on the next page is kept (it starts at this page's granule) */
        size_t pos = 0, start = 0;
        for (size_t i = 0; i < pg.lacing.size(); ++i) {
            pos += pg.lacing[i];
            if (pg.lacing[i] < 255) start = pos;
        }
        if (!pg.lacing.empty() && pg.lacing.back() == 255) {
            partial_.assign(pg.body.begin() + static_cast<long>(start), pg.body.end());
            partial_ok_ = true;
        } else {
            partial_ok_ = true; /* the next page starts with a fresh packet */
        }
    }
    return 0;
}

int OggOpusFile::seek(int64_t target48) {
    if (fd_ < 0) return -EBADF;
    st_.seeks++;
    if (!has_end_) return rewind();
    uint64_t lo = audio_off_, hi = size_;
    /* hints from pages seen before */
    auto it = known_.upper_bound(target48);
    if (it != known_.end()) hi = std::min<uint64_t>(hi, it->second + 1);
    if (it != known_.begin()) {
        --it;
        lo = std::max<uint64_t>(lo, it->second);
    }
    Page best;
    bool have_best = false;
    Page pg;
    int guard = 0;
    while (hi > lo && hi - lo > kLinearSpan && guard++ < 64) {
        const uint64_t mid = lo + (hi - lo) / 2;
        bool found = false;
        uint64_t from = mid;
        while (rd_->find_next(from, hi, pg, false)) {
            if (pg.serial == serial_ && pg.granule != kNoGranule) {
                found = true;
                break;
            }
            from = pg.end();
        }
        st_.probes++;
        if (rd_->error()) return rd_->error();
        if (!found) {
            hi = mid;
            continue;
        }
        note(pg);
        if (static_cast<int64_t>(pg.granule) <= target48) {
            best = pg;
            have_best = true;
            lo = pg.end();
        } else {
            hi = mid;
        }
    }
    /* the last stretch page by page */
    uint64_t off = lo;
    while (rd_->find_next(off, size_, pg, false)) {
        st_.probes++;
        if (pg.serial == serial_ && pg.granule != kNoGranule) {
            note(pg);
            if (static_cast<int64_t>(pg.granule) <= target48) {
                best = pg;
                have_best = true;
            } else {
                break;
            }
        }
        off = pg.end();
    }
    if (rd_->error()) return rd_->error();
    if (!have_best) return rewind();
    return start_at_page(best, true);
}

int OggOpusFile::next(Packet &out) {
    if (fd_ < 0) return -EBADF;
    for (;;) {
        if (ready_i_ < ready_.size()) {
            out = std::move(ready_[ready_i_++]);
            return 1;
        }
        if (at_end_) return 0;
        Page pg;
        if (!load_page(next_off_, pg)) {
            at_end_ = true;
            return rd_->error();
        }
        next_off_ = pg.end();
        if (pg.flags & 0x04) at_end_ = true;
        /* split the body into packets */
        ready_.clear();
        ready_i_ = 0;
        struct Raw {
            std::vector<unsigned char> data;
            bool ok;
        };
        std::vector<Raw> done;
        bool cur_ok;
        if (!(pg.flags & 0x01)) { /* a fresh packet starts here: an unfinished one before is lost */
            partial_.clear();
            cur_ok = true;
        } else { /* the page continues a packet: whole only when we hold its beginning */
            cur_ok = !partial_.empty() && partial_ok_;
        }
        std::vector<unsigned char> cur = std::move(partial_);
        partial_.clear();
        size_t pos = 0;
        for (size_t i = 0; i < pg.lacing.size(); ++i) {
            const size_t n = pg.lacing[i];
            if (pos + n > pg.body.size()) break;
            cur.insert(cur.end(), pg.body.begin() + static_cast<long>(pos), pg.body.begin() + static_cast<long>(pos + n));
            pos += n;
            if (n < 255) {
                done.push_back(Raw{std::move(cur), cur_ok});
                cur.clear();
                cur_ok = true;
            }
        }
        partial_ = std::move(cur);
        partial_ok_ = cur_ok;
        if (done.empty()) continue;
        if (pg.granule == kNoGranule) continue; /* packets completing on a page without a granule: not a valid stream */
        /* positions: backwards from the page's granule, except on the end-of-stream page, whose granule may trim the
         * end (RFC 7845 section 4.4): there forwards from the end of the page before (or from 0 when the stream is
         * that one page and its granule is smaller than its packets) */
        std::vector<uint32_t> dur(done.size());
        int64_t sum = 0;
        for (size_t i = 0; i < done.size(); ++i) {
            dur[i] = done[i].ok ? packet_samples48(done[i].data.data(), done[i].data.size()) : 0;
            sum += dur[i];
        }
        const int64_t g = static_cast<int64_t>(pg.granule);
        int64_t start = g - sum;
        if ((pg.flags & 0x04) && done[0].ok) {
            if (have_last_end_) start = last_end48_;
            else if (start < 0) start = 0;
        }
        std::vector<Packet> pk(done.size());
        int64_t at = start;
        for (size_t i = 0; i < done.size(); ++i) {
            pk[i].start48 = at;
            pk[i].end48 = at + dur[i];
            pk[i].page_offset = pg.offset;
            pk[i].data = std::move(done[i].data);
            if (!done[i].ok || dur[i] == 0) pk[i].data.clear(); /* a lost beginning or a malformed packet: dropped */
            at += dur[i];
        }
        last_end48_ = (pg.flags & 0x04) ? at : g;
        have_last_end_ = true;
        for (Packet &p : pk)
            if (!p.data.empty()) ready_.push_back(std::move(p));
    }
}

/* ---------------------------------------------------------------- remux */

int remux_opus_range(OggOpusFile &src, const RemuxRequest &req, ByteSink &sink, RemuxStats *stats,
                     const std::function<void(uint64_t, uint64_t)> &progress, const std::atomic<bool> *cancel) {
    if (!src.is_open() || req.rate_k == 0) return -EBADF;
    Timeline tl;
    tl.start48 = src.start48();
    tl.preskip = src.head().preskip;
    tl.k = req.rate_k;
    const uint64_t len = tl.length(src.end48());
    if (req.frames == 0 || req.first + req.frames > len) return kCodecBadStream;
    const int64_t t_in = tl.pos48(req.first), t_out = tl.pos48(req.first + req.frames);
    const int64_t pre = t_in - static_cast<int64_t>(kRemuxPreroll48);
    int r = src.seek(pre);
    if (r < 0) return r;
    /* the first packet: the latest that starts at or before the pre-roll point (or the stream's first) */
    std::vector<Packet> pend;
    Packet p;
    for (;;) {
        r = src.next(p);
        if (r < 0) return r;
        if (r == 0) break;
        if (p.start48 <= pre) {
            pend.clear();
            pend.push_back(std::move(p));
            continue;
        }
        pend.push_back(std::move(p));
        break;
    }
    if (pend.empty()) return kCodecBadStream;
    while (pend.size() > 1 && t_in - pend.front().start48 > 65535) pend.erase(pend.begin()); /* pre-skip is 16 bits */
    const int64_t base = pend.front().start48;
    if (base > t_in || t_in - base > 65535) return kCodecBadStream;
    RemuxStats s;
    s.preskip = static_cast<uint32_t>(t_in - base);
    s.granule_end = static_cast<uint64_t>(t_out - base);

    uint32_t seq = 0;
    auto out = [&](const std::vector<unsigned char> &page) {
        const int e = sink.write(page.data(), page.size());
        if (e < 0) return e;
        s.pages++;
        s.bytes += page.size();
        return 0;
    };
    Head h = src.head();
    h.preskip = s.preskip;
    r = out(build_page(0x02, 0, req.serial, seq++, {build_head(h)}));
    if (r < 0) return r;
    std::vector<unsigned char> tags = build_tags(req.vendor, req.tags);
    if (tags.size() > 255u * 255u - 1u) tags = build_tags(req.vendor, Tags()); /* a comment block this large: dropped */
    r = out(build_page(0, 0, req.serial, seq++, {tags}));
    if (r < 0) return r;

    /* audio pages of whole packets: at most about one second each, and the 255 lacing values of a page */
    std::vector<std::vector<unsigned char>> page;
    size_t segs = 0;
    int64_t page_start = base, page_end = base;
    size_t pi = 0;
    for (;;) {
        if (cancel && cancel->load()) return -ECANCELED;
        Packet q;
        if (pi < pend.size()) {
            q = std::move(pend[pi++]);
        } else {
            r = src.next(q);
            if (r < 0) return r;
            if (r == 0) return kCodecBadStream; /* the source ended before the range did */
        }
        if (q.start48 >= t_out) return kCodecBadStream; /* cannot happen: the loop ends at the packet containing t_out */
        const bool last = q.end48 >= t_out;
        const size_t need = q.data.size() / 255 + 1;
        if (!page.empty() && (segs + need > 255 || page_end - page_start >= 48000)) {
            r = out(build_page(0, static_cast<uint64_t>(page_end - base), req.serial, seq++, page));
            if (r < 0) return r;
            page.clear();
            segs = 0;
            page_start = page_end;
        }
        if (page.empty()) page_start = q.start48;
        segs += need;
        page_end = q.end48;
        page.push_back(std::move(q.data));
        s.packets++;
        if (progress) {
            const int64_t done48 = std::min(page_end, t_out) - t_in;
            progress(done48 > 0 ? static_cast<uint64_t>(done48) / tl.k : 0, req.frames);
        }
        if (last) {
            r = out(build_page(0x04, s.granule_end, req.serial, seq++, page));
            if (r < 0) return r;
            break;
        }
    }
    if (stats) *stats = s;
    return 0;
}

}  // namespace oggopus
}  // namespace cassette
