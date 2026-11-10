/* SPDX-License-Identifier: MIT */
#include "decoded_source.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace cassette {

void DecodedSource::close() {
    if (dec_) dec_->close();
    dec_.reset();
    info_ = DecodedInfo();
    align_ = 0;
    cache_.clear();
    c0_ = cn_ = dpos_ = 0;
}

int DecodedSource::open(const std::string &path) {
    close();
    DecoderLibs real;
    if (!libs_) real = real_decoder_libs();
    int err = 0;
    dec_ = make_decoder(ops_, path, libs_ ? *libs_ : real, info_, &err);
    if (!dec_) {
        info_ = DecodedInfo();
        return err < 0 ? err : kCodecBadStream;
    }
    align_ = static_cast<uint32_t>(info_.output.channels) * (info_.output.bits / 8u);
    st_ = Stats();
    return 0;
}

uint64_t DecodedSource::window_frames() const {
    return std::max<uint64_t>(info_.output.rate / 2, 16384);
}

int DecodedSource::extend(uint64_t end) {
    while (c0_ + cn_ < end) {
        const uint64_t chunk = std::min<uint64_t>(end - (c0_ + cn_), 8192);
        cache_.resize(static_cast<size_t>(cn_ + chunk) * align_);
        const long got = dec_->read(cache_.data() + cn_ * align_, static_cast<size_t>(chunk));
        if (got < 0) {
            cache_.resize(static_cast<size_t>(cn_) * align_);
            return static_cast<int>(got);
        }
        cn_ += static_cast<uint64_t>(got);
        dpos_ += static_cast<uint64_t>(got);
        st_.decoded += static_cast<uint64_t>(got);
        cache_.resize(static_cast<size_t>(cn_) * align_);
        if (got == 0) break;
    }
    return 0;
}

long DecodedSource::read(uint64_t frame, size_t n, void *buf) {
    if (!dec_) return -EBADF;
    const uint64_t total = info_.total_frames;
    if (frame >= total || n == 0) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, total - frame));
    const uint64_t end = frame + n;
    st_.requested += n;
    const uint64_t W = window_frames();
    const bool inside = cn_ > 0 && frame >= c0_ && end <= c0_ + cn_;
    if (!inside) {
        if (cn_ > 0 && frame >= c0_ && frame <= c0_ + cn_ && dpos_ == c0_ + cn_) {
            /* reading on: decode what is missing */
        } else if (cn_ == 0 && frame == dpos_ && dec_->position() == dpos_) {
            c0_ = dpos_; /* the decoder stands where the read starts (just opened) */
        } else {
            const bool reverse = cn_ > 0 && frame < c0_ && end + 2 * W >= c0_ && end <= c0_ + cn_;
            uint64_t ws = frame;
            if (reverse) {
                ws = end > W ? end - W : 0;
                ws = std::min(ws, frame);
                st_.reverse++;
            }
            uint64_t landed = ws;
            const int r = dec_->seek_near(ws, landed);
            st_.seeks++;
            cache_.clear();
            cn_ = 0;
            if (r < 0) {
                c0_ = dpos_ = 0;
                return r;
            }
            c0_ = dpos_ = landed;
        }
        const int r = extend(end);
        if (r < 0) return r;
        /* keep the window bounded: drop the oldest frames, down to one window before the read */
        const uint64_t keep_max = std::max<uint64_t>(4 * W, n + W);
        if (cn_ > keep_max && frame > c0_) {
            const uint64_t drop = std::min<uint64_t>(frame - c0_, cn_ - std::min<uint64_t>(cn_, W + n));
            if (drop > 0) {
                cache_.erase(cache_.begin(), cache_.begin() + static_cast<long>(drop * align_));
                c0_ += drop;
                cn_ -= drop;
            }
        }
    }
    if (frame < c0_ || frame >= c0_ + cn_) return 0;
    const uint64_t have = std::min<uint64_t>(end, c0_ + cn_) - frame;
    std::memcpy(buf, cache_.data() + (frame - c0_) * align_, static_cast<size_t>(have) * align_);
    return static_cast<long>(have);
}

}  // namespace cassette
