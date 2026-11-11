/* SPDX-License-Identifier: MIT */
#include "opus_decoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace cassette {

void OpusFileDecoder::close() {
    if (dec_ && lib_) lib_->decoder_destroy(dec_);
    dec_ = nullptr;
    file_.reset();
    pcm_.clear();
    pcm_off_ = 0;
    have_pending_ = false;
}

int OpusFileDecoder::open(FileOps &ops, const std::string &path, DecodedInfo &info) {
    close();
    if (!lib_ || !lib_->complete()) return kCodecMissing;
    file_.reset(new oggopus::OggOpusFile(ops));
    const int r = file_->open(path);
    if (r < 0) {
        file_.reset();
        return r;
    }
    const oggopus::Head &h = file_->head();
    if (h.mapping_family != 0 || h.channels < 1 || h.channels > 2) { /* Cassette records mono or stereo, family 0 */
        file_.reset();
        return kCodecBadStream;
    }
    ch_ = h.channels;
    rate_ = oggopus::decode_rate(h.input_rate);
    tl_.start48 = file_->start48();
    tl_.preskip = h.preskip;
    tl_.k = 48000u / rate_;
    total_ = tl_.length(file_->end48());
    int err = 0;
    dec_ = lib_->decoder_create(static_cast<int32_t>(rate_), static_cast<int>(ch_), &err);
    if (!dec_) {
        file_.reset();
        return kCodecFailed;
    }
    if (h.gain_q8 != 0) lib_->decoder_ctl(dec_, opus::kSetGainRequest, static_cast<int32_t>(h.gain_q8));
    tmp_.assign(static_cast<size_t>(oggopus::kMaxPacket48 / tl_.k) * ch_, 0);
    info = DecodedInfo();
    info.kind = DecodedKind::OggOpus;
    info.source.rate = h.input_rate ? h.input_rate : 48000;
    info.source.channels = static_cast<uint16_t>(ch_);
    info.source.bits = 16;
    info.output.rate = rate_;
    info.output.channels = static_cast<uint16_t>(ch_);
    info.output.bits = 16;
    info.total_frames = total_;
    info.frames_known = file_->has_audio();
    info.tags = file_->tags();
    info.vendor = file_->vendor();
    info.damaged = file_->has_audio() && !file_->eos();
    reset_buffers(0);
    eos_ = total_ == 0;
    return 0;
}

void OpusFileDecoder::reset_buffers(uint64_t frame) {
    pcm_.clear();
    pcm_off_ = 0;
    pos_ = want_ = next_out_ = frame;
}

int OpusFileDecoder::decode_next() {
    oggopus::Packet p;
    if (have_pending_) {
        p = std::move(pending_);
        have_pending_ = false;
    } else {
        const int r = file_->next(p);
        if (r < 0) return r;
        if (r == 0) {
            eos_ = true;
            return 0;
        }
    }
    st_.packets++;
    const int max_frames = static_cast<int>(oggopus::kMaxPacket48 / tl_.k);
    const int expect = static_cast<int>(oggopus::packet_samples48(p.data.data(), p.data.size()) / tl_.k);
    int n = lib_->decode(dec_, p.data.data(), static_cast<int32_t>(p.data.size()), tmp_.data(), max_frames, 0);
    if (n < 0 || n != expect) { /* a packet libopus refuses (or that disagrees with its TOC): conceal its duration */
        st_.concealed++;
        n = lib_->decode(dec_, nullptr, 0, tmp_.data(), expect, 0);
        if (n != expect) {
            std::fill(tmp_.begin(), tmp_.begin() + static_cast<long>(expect) * ch_, 0);
            n = expect;
        }
    }
    st_.frames_decoded += static_cast<uint64_t>(n);
    const int64_t o0 = tl_.frame_of(p.start48);
    /* keep [max(want_, next_out_), total_) */
    for (int j = 0; j < n; ++j) {
        const int64_t o = o0 + j;
        if (o < 0 || static_cast<uint64_t>(o) < want_ || static_cast<uint64_t>(o) < next_out_) continue;
        if (static_cast<uint64_t>(o) >= total_) break;
        if (static_cast<uint64_t>(o) > next_out_) { /* a hole (pages lost to damage): silence keeps the positions */
            const uint64_t gap = std::min<uint64_t>(static_cast<uint64_t>(o) - next_out_, uint64_t(1) << 24);
            pcm_.insert(pcm_.end(), static_cast<size_t>(gap) * ch_, 0);
            next_out_ += gap;
        }
        pcm_.insert(pcm_.end(), tmp_.begin() + static_cast<long>(j) * ch_, tmp_.begin() + static_cast<long>(j + 1) * ch_);
        ++next_out_;
    }
    return 1;
}

long OpusFileDecoder::read(void *out, size_t frames) {
    if (!dec_) return -EBADF;
    unsigned char *o = static_cast<unsigned char *>(out);
    size_t done = 0;
    while (done < frames && pos_ < total_) {
        const size_t avail = pcm_.size() / ch_ - pcm_off_;
        if (avail == 0) {
            pcm_.clear();
            pcm_off_ = 0;
            if (eos_) break;
            const int r = decode_next();
            if (r < 0) return done ? static_cast<long>(done) : r;
            if (r == 0) break;
            continue;
        }
        const size_t take = static_cast<size_t>(std::min<uint64_t>(std::min<uint64_t>(avail, frames - done), total_ - pos_));
        const int16_t *s = pcm_.data() + pcm_off_ * ch_;
        for (size_t i = 0; i < take * ch_; ++i) {
            const uint16_t u = static_cast<uint16_t>(s[i]);
            *o++ = static_cast<unsigned char>(u & 0xFF);
            *o++ = static_cast<unsigned char>(u >> 8);
        }
        pcm_off_ += take;
        pos_ += take;
        done += take;
    }
    return static_cast<long>(done);
}

int OpusFileDecoder::seek_near(uint64_t frame, uint64_t &landed) {
    if (!dec_) return -EBADF;
    st_.seeks++;
    have_pending_ = false;
    if (frame >= total_) {
        reset_buffers(total_);
        eos_ = true;
        landed = total_;
        return 0;
    }
    eos_ = false;
    lib_->decoder_ctl(dec_, opus::kResetState);
    const int64_t pre = tl_.pos48(frame) - static_cast<int64_t>(oggopus::kPreroll48);
    int r = file_->seek(pre);
    if (r < 0) return r;
    r = file_->next(pending_);
    if (r < 0) return r;
    if (r == 0) {
        reset_buffers(total_);
        eos_ = true;
        landed = total_;
        return 0;
    }
    have_pending_ = true;
    /* the frames from the first packet's start plus the pre-roll on are converged; from the stream start, all are */
    int64_t from = 0;
    if (pending_.start48 > tl_.start48) from = tl_.frame_of(pending_.start48 + oggopus::kPreroll48);
    if (from < 0) from = 0;
    landed = std::min<uint64_t>(static_cast<uint64_t>(from), frame);
    reset_buffers(landed);
    return 0;
}

int OpusFileDecoder::seek(uint64_t frame) {
    uint64_t landed = 0;
    const int r = seek_near(frame, landed);
    if (r < 0) return r;
    if (frame < total_) {
        pos_ = want_ = next_out_ = frame; /* decode and drop up to the target */
    }
    return 0;
}

}  // namespace cassette
