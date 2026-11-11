/* SPDX-License-Identifier: MIT */
#include "flac_encoder.hpp"

#include <algorithm>
#include <cstdlib>

namespace cassette {

bool flac_format_ok(const AudioFormat &fmt, std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    if (fmt.bits != 16 && fmt.bits != 24) return no("FLAC records 16 or 24 bits");
    if (fmt.channels < 1 || fmt.channels > 2) return no("more than 2 channels");
    /* the subset's frame header codes any rate in Hz up to 65535, or in 10 Hz or 1 kHz steps above (RFC 9639 9.1.2);
     * libFLAC also refuses rates above 655350 Hz in the subset */
    if (fmt.rate == 0 || fmt.rate > 655350) return no("sample rate FLAC cannot store");
    if (fmt.rate > 65535 && fmt.rate % 10 != 0) return no("sample rate FLAC cannot store");
    return true;
}

FlacEncoder::FlacEncoder(const FlacLib *lib, const FlacConfig &cfg) : lib_(lib), cfg_(cfg) {
    cfg_.level = std::min(cfg_.level, 8u);
}

FlacEncoder::~FlacEncoder() {
    detached_ = true; /* FLAC__stream_encoder_delete finishes an unfinished stream: its writes go nowhere */
    if (lib_ && enc_) lib_->encoder_delete(enc_);
    if (lib_)
        for (flac::Metadata *m : meta_) lib_->metadata_delete(m);
}

int FlacEncoder::fail(int err) {
    if (!st_.error) st_.error = err;
    return st_.error;
}

int FlacEncoder::lib_error() { return fail(sink_err_ ? sink_err_ : kCodecFailed); }

int FlacEncoder::write_cb(const flac::Encoder *, const uint8_t *buf, size_t bytes, uint32_t samples, uint32_t,
                          void *client) {
    FlacEncoder *self = static_cast<FlacEncoder *>(client);
    if (self->detached_ || !self->sink_) return flac::kWriteFatal;
    int r = self->sink_->write(buf, bytes);
    if (r < 0) {
        if (!self->sink_err_) self->sink_err_ = r;
        return flac::kWriteFatal;
    }
    self->st_.bytes_out += bytes;
    self->st_.units_out++;
    self->st_.frames_out += samples;
    return flac::kWriteOk;
}

int FlacEncoder::seek_cb(const flac::Encoder *, uint64_t off, void *client) {
    FlacEncoder *self = static_cast<FlacEncoder *>(client);
    if (self->detached_ || !self->sink_) return flac::kSeekError;
    int r = self->sink_->seek(off);
    if (r < 0) {
        if (!self->sink_err_) self->sink_err_ = r;
        return flac::kSeekError;
    }
    return flac::kSeekOk;
}

int FlacEncoder::tell_cb(const flac::Encoder *, uint64_t *off, void *client) {
    FlacEncoder *self = static_cast<FlacEncoder *>(client);
    if (self->detached_ || !self->sink_) return flac::kTellError;
    *off = self->sink_->tell();
    return flac::kTellOk;
}

int FlacEncoder::start(ByteSink &sink, const Tags &tags) {
    if (st_.error) return st_.error;
    if (started_) return fail(kCodecState);
    if (!lib_ || !lib_->complete()) return fail(kCodecMissing);
    if (!flac_format_ok(cfg_.fmt)) return fail(kCodecRefused);
    for (const Tag &t : tags)
        if (!valid_tag_name(t.name)) return fail(kCodecRefused);

    enc_ = lib_->encoder_new();
    if (!enc_) return fail(kCodecFailed);
    bool ok = lib_->set_verify(enc_, cfg_.verify ? 1 : 0) && lib_->set_streamable_subset(enc_, 1) &&
              lib_->set_channels(enc_, cfg_.fmt.channels) && lib_->set_bits_per_sample(enc_, cfg_.fmt.bits) &&
              lib_->set_sample_rate(enc_, cfg_.fmt.rate) && lib_->set_compression_level(enc_, cfg_.level);
    if (!ok) return fail(kCodecRefused);

    flac::Metadata *vc = lib_->metadata_new(flac::kMetaVorbisComment);
    if (!vc) return fail(kCodecFailed);
    meta_.push_back(vc);
    for (const Tag &t : tags) {
        flac::VorbisCommentEntry e{0, nullptr};
        if (!lib_->vc_entry_from_pair(&e, t.name.c_str(), t.value.c_str())) return fail(kCodecFailed);
        if (!lib_->vc_append_comment(vc, e, 0)) { /* copy=false: libFLAC owns the entry on success only */
            std::free(e.entry);
            return fail(kCodecFailed);
        }
    }
    if (cfg_.padding_bytes > 0) {
        flac::Metadata *pad = lib_->metadata_new(flac::kMetaPadding);
        if (!pad) return fail(kCodecFailed);
        reinterpret_cast<flac::MetadataHead *>(pad)->length = cfg_.padding_bytes;
        meta_.push_back(pad);
    }
    if (!lib_->set_metadata(enc_, meta_.data(), static_cast<uint32_t>(meta_.size()))) return fail(kCodecRefused);

    scratch_.assign(kChunkFrames * cfg_.fmt.channels, 0);
    sink_ = &sink;
    const int st = lib_->init_stream(enc_, &FlacEncoder::write_cb, &FlacEncoder::seek_cb, &FlacEncoder::tell_cb,
                                     nullptr, this);
    if (st != flac::kInitOk) {
        if (sink_err_) return fail(sink_err_);
        return fail(st == flac::kInitEncoderError ? kCodecFailed : kCodecRefused);
    }
    started_ = true;
    return 0;
}

int FlacEncoder::write(const void *pcm, size_t frames) {
    if (st_.error) return st_.error;
    if (!started_ || st_.finished) return fail(kCodecState);
    const unsigned char *p = static_cast<const unsigned char *>(pcm);
    const size_t fb = static_cast<size_t>(cfg_.fmt.channels) * (cfg_.fmt.bits / 8u);
    while (frames > 0) {
        const size_t n = std::min(frames, kChunkFrames);
        pcm_to_int32(p, cfg_.fmt.bits, scratch_.data(), n * cfg_.fmt.channels);
        if (!lib_->process_interleaved(enc_, scratch_.data(), static_cast<uint32_t>(n))) return lib_error();
        st_.frames_in += n;
        p += n * fb;
        frames -= n;
    }
    return 0;
}

int FlacEncoder::flush() {
    if (st_.error) return st_.error;
    if (!started_) return fail(kCodecState);
    return 0; /* every complete frame went to the sink during write(); the partial block can only leave with finish() */
}

int FlacEncoder::finish() {
    if (st_.error) return st_.error;
    if (!started_) return fail(kCodecState);
    if (st_.finished) return 0;
    st_.finished = true;
    if (!lib_->finish(enc_)) return lib_error();
    if (sink_err_) return fail(sink_err_);
    return 0;
}

}  // namespace cassette
