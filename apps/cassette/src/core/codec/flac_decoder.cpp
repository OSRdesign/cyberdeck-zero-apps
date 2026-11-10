/* SPDX-License-Identifier: MIT */
#include "flac_decoder.hpp"

#include "stream_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace cassette {
namespace {

uint32_t le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int64_t pread_all(FileOps &ops, int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        const ssize_t k = ops.pread(fd, static_cast<char *>(buf) + got, n - got, off + got);
        if (k == -EINTR) continue;
        if (k < 0) return k;
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    return static_cast<int64_t>(got);
}

/* The metadata blocks: STREAMINFO's block sizes and the Vorbis comments. false when the header cannot be read. */
bool read_flac_header(FileOps &ops, int fd, uint64_t size, uint32_t &min_block, uint32_t &max_block, std::string &vendor,
                      Tags &tags, int &err) {
    unsigned char h[4];
    if (pread_all(ops, fd, h, 4, 0) != 4 || std::memcmp(h, "fLaC", 4) != 0) return false;
    uint64_t pos = 4;
    bool last = false;
    while (!last) {
        int64_t k = pread_all(ops, fd, h, 4, pos);
        if (k < 0) err = static_cast<int>(k);
        if (k != 4) return false;
        last = (h[0] & 0x80) != 0;
        const unsigned type = h[0] & 0x7F;
        const uint32_t len = (static_cast<uint32_t>(h[1]) << 16) | (h[2] << 8) | h[3];
        if (pos + 4 + len > size) return false;
        if (type == 0 && len >= 4) {
            unsigned char b[4];
            k = pread_all(ops, fd, b, 4, pos + 4);
            if (k != 4) return false;
            min_block = (b[0] << 8) | b[1];
            max_block = (b[2] << 8) | b[3];
        } else if (type == 4 && len >= 8 && len < (1u << 20)) {
            std::vector<unsigned char> b(len);
            k = pread_all(ops, fd, b.data(), len, pos + 4);
            if (k != static_cast<int64_t>(len)) return false;
            size_t q = 0;
            const uint32_t vl = le32(b.data());
            q += 4;
            if (vl <= len - q) {
                vendor.assign(reinterpret_cast<const char *>(b.data() + q), vl);
                q += vl;
                if (len - q >= 4) {
                    const uint32_t n = le32(b.data() + q);
                    q += 4;
                    for (uint32_t i = 0; i < n && len - q >= 4; ++i) {
                        const uint32_t l = le32(b.data() + q);
                        q += 4;
                        if (l > len - q) break;
                        const std::string s(reinterpret_cast<const char *>(b.data() + q), l);
                        q += l;
                        const size_t eq = s.find('=');
                        if (eq != std::string::npos && eq > 0) tags.push_back(Tag{s.substr(0, eq), s.substr(eq + 1)});
                    }
                }
            }
        }
        pos += 4 + static_cast<uint64_t>(len);
    }
    return true;
}

void store(unsigned char *p, unsigned bytes, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    p[0] = static_cast<unsigned char>(u & 0xFF);
    p[1] = static_cast<unsigned char>((u >> 8) & 0xFF);
    if (bytes >= 3) p[2] = static_cast<unsigned char>((u >> 16) & 0xFF);
    if (bytes >= 4) p[3] = static_cast<unsigned char>((u >> 24) & 0xFF);
}

}  // namespace

bool read_flac_tags(FileOps &ops, const std::string &path, std::string &vendor, Tags &tags, int *err) {
    vendor.clear();
    tags.clear();
    const int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0) {
        if (err) *err = fd;
        return false;
    }
    StatInfo st;
    int e = ops.fstat(fd, st);
    bool ok = false;
    if (e == 0) {
        uint32_t a = 0, b = 0;
        ok = read_flac_header(ops, fd, st.size, a, b, vendor, tags, e);
    }
    ops.close(fd);
    if (err) *err = e;
    return ok;
}

/* ---------------------------------------------------------------- callbacks */

int FlacDecoder::read_cb(const flac::Decoder *, uint8_t buffer[], size_t *bytes, void *client) {
    FlacDecoder *self = static_cast<FlacDecoder *>(client);
    if (*bytes == 0) return flac::kReadAbort;
    if (self->fpos_ >= self->size_) {
        *bytes = 0;
        return flac::kReadEndOfStream;
    }
    const size_t want = static_cast<size_t>(std::min<uint64_t>(*bytes, self->size_ - self->fpos_));
    for (;;) {
        const ssize_t k = self->ops_->pread(self->fd_, buffer, want, self->fpos_);
        if (k == -EINTR) continue;
        if (k < 0) {
            if (!self->io_err_) self->io_err_ = static_cast<int>(k);
            *bytes = 0;
            return flac::kReadAbort;
        }
        if (k == 0) {
            *bytes = 0;
            return flac::kReadEndOfStream;
        }
        *bytes = static_cast<size_t>(k);
        self->fpos_ += static_cast<uint64_t>(k);
        return flac::kReadContinue;
    }
}

int FlacDecoder::seek_cb(const flac::Decoder *, uint64_t off, void *client) {
    FlacDecoder *self = static_cast<FlacDecoder *>(client);
    if (off > self->size_) return flac::kDecSeekStatusError;
    self->fpos_ = off;
    return flac::kDecSeekStatusOk;
}

int FlacDecoder::tell_cb(const flac::Decoder *, uint64_t *off, void *client) {
    *off = static_cast<FlacDecoder *>(client)->fpos_;
    return flac::kDecTellOk;
}

int FlacDecoder::length_cb(const flac::Decoder *, uint64_t *len, void *client) {
    *len = static_cast<FlacDecoder *>(client)->size_;
    return flac::kLengthOk;
}

flac::Bool FlacDecoder::eof_cb(const flac::Decoder *, void *client) {
    FlacDecoder *self = static_cast<FlacDecoder *>(client);
    return self->fpos_ >= self->size_ ? 1 : 0;
}

int FlacDecoder::write_cb(const flac::Decoder *, const flac::FrameHeader *h, const int32_t *const buffer[], void *client) {
    FlacDecoder *self = static_cast<FlacDecoder *>(client);
    const unsigned ch = self->src_.channels;
    if (h->channels != ch) return flac::kWriteAbort;
    const uint64_t n = h->blocksize;
    const uint64_t start = h->number_type == flac::kFrameNumberTypeSample
                               ? h->number.sample_number
                               : static_cast<uint64_t>(h->number.frame_number) * self->nominal_block_;
    self->st_.frames_decoded += n;
    uint64_t skip = 0;
    if (start > self->expect_) { /* frames lost to damage: silence keeps the positions */
        const uint64_t gap = std::min<uint64_t>(start - self->expect_, uint64_t(1) << 24);
        self->pend_.insert(self->pend_.end(), static_cast<size_t>(gap) * ch, 0);
        self->st_.gaps += gap;
        self->damaged_ = true;
        self->expect_ += gap;
        if (start > self->expect_) skip = n; /* an absurd jump: drop the frame */
    } else {
        skip = self->expect_ - start;
    }
    if (skip >= n) return flac::kWriteContinue;
    const size_t base = self->pend_.size();
    self->pend_.resize(base + static_cast<size_t>(n - skip) * ch);
    int32_t *o = self->pend_.data() + base;
    for (uint64_t i = skip; i < n; ++i)
        for (unsigned c = 0; c < ch; ++c) *o++ = buffer[c][i];
    self->expect_ = start + n;
    return flac::kWriteContinue;
}

void FlacDecoder::error_cb(const flac::Decoder *, int, void *client) { static_cast<FlacDecoder *>(client)->damaged_ = true; }

/* ---------------------------------------------------------------- the decoder */

void FlacDecoder::close() {
    if (dec_ && lib_) {
        if (inited_) lib_->finish(dec_);
        lib_->decoder_delete(dec_);
    }
    dec_ = nullptr;
    inited_ = false;
    if (fd_ >= 0 && ops_) ops_->close(fd_);
    fd_ = -1;
    pend_.clear();
    pend_off_ = 0;
}

int FlacDecoder::open(FileOps &ops, const std::string &path, DecodedInfo &info) {
    close();
    if (!lib_ || !lib_->complete()) return kCodecMissing;
    ops_ = &ops;
    path_ = path;
    const int fd = ops.open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    fd_ = fd;
    const StreamInfo si = read_stream_info(ops, path);
    if (!si.ok || si.kind != Container::Flac || si.fmt.channels < 1 || si.fmt.channels > 8 || si.fmt.bits < 4 ||
        si.fmt.bits > 32) {
        close();
        return kCodecBadStream;
    }
    StatInfo st;
    int r = ops.fstat(fd, st);
    if (r < 0) {
        close();
        return r;
    }
    size_ = st.size;
    fpos_ = 0;
    io_err_ = 0;
    uint32_t min_block = 0, max_block = 0;
    info = DecodedInfo();
    int herr = 0;
    if (!read_flac_header(ops, fd, size_, min_block, max_block, info.vendor, info.tags, herr)) {
        close();
        return herr ? herr : kCodecBadStream;
    }
    nominal_block_ = min_block == max_block ? max_block : 0;
    src_ = si.fmt;
    out_ = si.fmt;
    out_.bits = src_.bits <= 16 ? 16 : src_.bits <= 24 ? 24 : 32;
    out_bytes_ = out_.bits / 8u;
    shift_ = out_.bits - src_.bits;
    total_ = si.frames;
    info.frames_known = si.frames_known;
    if (!si.frames_known) { /* STREAMINFO says "unknown": count the valid frames */
        const StreamScan sc = scan_flac(ops, path);
        if (!sc.ok) {
            close();
            return kCodecBadStream;
        }
        total_ = sc.frames;
        info.damaged = true;
    }
    dec_ = lib_->decoder_new();
    if (!dec_) {
        close();
        return kCodecFailed;
    }
    lib_->set_md5_checking(dec_, 0);
    if (lib_->init_stream(dec_, &FlacDecoder::read_cb, &FlacDecoder::seek_cb, &FlacDecoder::tell_cb,
                          &FlacDecoder::length_cb, &FlacDecoder::eof_cb, &FlacDecoder::write_cb, nullptr,
                          &FlacDecoder::error_cb, this) != flac::kDecInitOk) {
        close();
        return kCodecFailed;
    }
    inited_ = true;
    if (!lib_->process_until_end_of_metadata(dec_)) {
        const int e = io_err_ ? io_err_ : kCodecBadStream;
        close();
        return e;
    }
    pos_ = expect_ = 0;
    pend_.clear();
    pend_off_ = 0;
    eos_ = false;
    damaged_ = false;
    info.kind = DecodedKind::Flac;
    info.source = src_;
    info.output = out_;
    info.total_frames = total_;
    return 0;
}

int FlacDecoder::fill() {
    if (!dec_) return -EBADF;
    const size_t before = pend_.size();
    for (int tries = 0; tries < 256 && pend_.size() == before; ++tries) {
        if (!lib_->process_single(dec_)) {
            const int s = lib_->get_state(dec_);
            if (s == flac::kDecEndOfStream) {
                eos_ = true;
                break;
            }
            return io_err_ ? io_err_ : kCodecFailed;
        }
        if (lib_->get_state(dec_) == flac::kDecEndOfStream) {
            eos_ = true;
            break;
        }
        if (io_err_) return io_err_;
    }
    return pend_.size() > before ? 1 : 0;
}

long FlacDecoder::read(void *out, size_t frames) {
    if (!dec_) return -EBADF;
    const unsigned ch = src_.channels;
    unsigned char *o = static_cast<unsigned char *>(out);
    size_t done = 0;
    while (done < frames && pos_ < total_) {
        size_t avail = pend_.size() / ch - pend_off_;
        if (avail == 0) {
            pend_.clear();
            pend_off_ = 0;
            if (eos_) break;
            const int r = fill();
            if (r < 0) return done ? static_cast<long>(done) : r;
            if (r == 0) break;
            continue;
        }
        const size_t take = static_cast<size_t>(std::min<uint64_t>(std::min<uint64_t>(avail, frames - done), total_ - pos_));
        const int32_t *s = pend_.data() + pend_off_ * ch;
        for (size_t i = 0; i < take * ch; ++i) {
            store(o, out_bytes_, static_cast<int32_t>(static_cast<uint32_t>(s[i]) << shift_));
            o += out_bytes_;
        }
        pend_off_ += take;
        pos_ += take;
        done += take;
    }
    return static_cast<long>(done);
}

int FlacDecoder::seek(uint64_t frame) {
    if (!dec_) return -EBADF;
    st_.seeks++;
    pend_.clear();
    pend_off_ = 0;
    if (frame >= total_) {
        pos_ = expect_ = total_;
        eos_ = true;
        return 0;
    }
    eos_ = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pend_.clear();
        pos_ = expect_ = frame;
        io_err_ = 0;
        if (lib_->seek_absolute(dec_, frame)) return 0; /* the target frame's tail is already in pend_ */
        if (io_err_) break;
        lib_->flush(dec_); /* SEEK_ERROR: flush, then once more */
    }
    pend_.clear();
    lib_->flush(dec_);
    return io_err_ ? io_err_ : kCodecFailed;
}

int FlacDecoder::scan_length(uint64_t &frames) {
    if (!ops_) return -EBADF;
    const StreamScan sc = scan_flac(*ops_, path_);
    if (!sc.ok) return kCodecBadStream;
    frames = sc.frames;
    return 0;
}

}  // namespace cassette
