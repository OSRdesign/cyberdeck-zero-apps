/* SPDX-License-Identifier: MIT */
#include "opus_encoder.hpp"

#include <algorithm>
#include <cstring>

namespace cassette {

bool opus_rate_ok(uint32_t rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

bool opus_format_ok(const AudioFormat &fmt, std::string *why) {
    auto no = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    if (fmt.channels < 1 || fmt.channels > 2) return no("Opus records 1 or 2 channels");
    if (!opus_rate_ok(fmt.rate)) return no("Opus needs 8, 12, 16, 24 or 48 kHz");
    if (fmt.bits != 16 && fmt.bits != 24) return no("Opus takes 16 or 24 bits");
    return true;
}

OpusEncoder::OpusEncoder(const OpusEncLib *lib, const OpusConfig &cfg) : lib_(lib), cfg_(cfg) {
    cfg_.complexity = std::clamp(cfg_.complexity, 0, 10);
    cfg_.bitrate_bps = std::clamp(cfg_.bitrate_bps, 6000, 510000);
}

OpusEncoder::~OpusEncoder() {
    if (lib_ && enc_) lib_->encoder_destroy(enc_);
    if (lib_ && comments_) lib_->comments_destroy(comments_);
}

int OpusEncoder::fail(int err) {
    if (!st_.error) st_.error = err;
    return st_.error;
}

namespace {

uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

}  // namespace

/* One Ogg page (RFC 3533): "OggS", version, header type (0x04 = end of stream), granule position (64-bit LE), serial,
 * sequence, CRC, segment count, lacing values, body. The first page's body is OpusHead (pre-skip at offset 10). */
int OpusEncoder::on_page(const unsigned char *page, size_t len) {
    int r = sink_->write(page, len);
    if (r < 0) return fail(r);
    st_.bytes_out += len;
    st_.units_out++;
    if (len < 27 || std::memcmp(page, "OggS", 4) != 0) return 0;
    const uint64_t g = le64(page + 6);
    if (page[5] & 0x04) eos_ = true;
    const size_t body = 27u + page[26];
    if (!have_head_ && len >= body + 12 && std::memcmp(page + body, "OpusHead", 8) == 0) {
        preskip_ = static_cast<uint32_t>(page[body + 10] | (page[body + 11] << 8));
        have_head_ = true;
    }
    if (g != ~0ull) {
        granule_ = g;
        if (g > preskip_) {
            const uint64_t out = (g - preskip_) * cfg_.fmt.rate / 48000u;
            st_.frames_out = std::min(out, st_.frames_in);
        }
    }
    return 0;
}

int OpusEncoder::pull(bool flush) {
    for (;;) {
        unsigned char *page = nullptr;
        int32_t len = 0;
        const int r = lib_->encoder_get_page(enc_, &page, &len, flush ? 1 : 0);
        if (r < 0) return fail(kCodecFailed);
        if (r == 0) return 0;
        if (len > 0 && page) {
            const int w = on_page(page, static_cast<size_t>(len));
            if (w < 0) return w;
        }
    }
}

int OpusEncoder::start(ByteSink &sink, const Tags &tags) {
    if (st_.error) return st_.error;
    if (started_) return fail(kCodecState);
    if (!lib_ || !lib_->complete()) return fail(kCodecMissing);
    if (!opus_format_ok(cfg_.fmt)) return fail(kCodecRefused);
    for (const Tag &t : tags)
        if (!valid_tag_name(t.name)) return fail(kCodecRefused);

    comments_ = lib_->comments_create();
    if (!comments_) return fail(kCodecFailed);
    for (const Tag &t : tags)
        if (lib_->comments_add(comments_, t.name.c_str(), t.value.c_str()) != ope::kOk) return fail(kCodecFailed);
    int err = 0;
    enc_ = lib_->encoder_create_pull(comments_, static_cast<int32_t>(cfg_.fmt.rate), cfg_.fmt.channels, 0, &err);
    if (!enc_) return fail(err == ope::kAllocFail ? kCodecFailed : kCodecRefused);

    const bool speech = cfg_.application == OpusApplication::Speech;
    auto ctl = [&](int req, int value) { return lib_->encoder_ctl(enc_, req, static_cast<int32_t>(value)) == 0; };
    bool ok = ctl(ope::kSetApplication, speech ? ope::kApplicationVoip : ope::kApplicationAudio) &&
              ctl(ope::kSetSignal, speech ? ope::kSignalVoice : ope::kSignalMusic) &&
              ctl(ope::kSetBitrate, cfg_.bitrate_bps) && ctl(ope::kSetVbr, cfg_.vbr ? 1 : 0) &&
              ctl(ope::kSetComplexity, cfg_.complexity) &&
              ctl(ope::kSetExpertFrameDuration, ope::kFramesize20Ms) &&
              ctl(ope::kSetDecisionDelay, static_cast<int>(cfg_.decision_delay_ms * 48u)) &&
              ctl(ope::kSetMuxingDelay, static_cast<int>(cfg_.muxing_delay_ms * 48u)) &&
              ctl(ope::kSetCommentPadding, std::max(cfg_.comment_padding, 0));
    if (ok && cfg_.force_mono && cfg_.fmt.channels == 2) ok = ctl(ope::kSetForceChannels, 1);
    if (ok && cfg_.serialno >= 0) ok = ctl(ope::kSetSerialno, static_cast<int>(cfg_.serialno));
    if (!ok) return fail(kCodecRefused);

    sink_ = &sink;
    if (lib_->encoder_flush_header(enc_) != ope::kOk) return fail(kCodecFailed);
    s16_.assign(kChunkFrames * cfg_.fmt.channels, 0);
    if (cfg_.fmt.bits == 24) f32_.assign(kChunkFrames * cfg_.fmt.channels, 0.0f);
    started_ = true;
    return pull(true); /* OpusHead and OpusTags pages: an empty take is a valid file */
}

int OpusEncoder::write(const void *pcm, size_t frames) {
    if (st_.error) return st_.error;
    if (!started_ || st_.finished) return fail(kCodecState);
    const unsigned char *p = static_cast<const unsigned char *>(pcm);
    const size_t fb = static_cast<size_t>(cfg_.fmt.channels) * (cfg_.fmt.bits / 8u);
    while (frames > 0) {
        const size_t n = std::min(frames, kChunkFrames);
        int r;
        if (cfg_.fmt.bits == 24) {
            pcm_to_float(p, 24, f32_.data(), n * cfg_.fmt.channels);
            r = lib_->encoder_write_float(enc_, f32_.data(), static_cast<int>(n));
        } else {
            pcm_to_int16(p, 16, s16_.data(), n * cfg_.fmt.channels);
            r = lib_->encoder_write(enc_, s16_.data(), static_cast<int>(n));
        }
        if (r != ope::kOk) return fail(kCodecFailed);
        st_.frames_in += n;
        p += n * fb;
        frames -= n;
        if (pull(false) < 0) return st_.error;
    }
    return 0;
}

int OpusEncoder::flush() {
    if (st_.error) return st_.error;
    if (!started_) return fail(kCodecState);
    if (st_.finished) return 0;
    return pull(true);
}

int OpusEncoder::finish() {
    if (st_.error) return st_.error;
    if (!started_) return fail(kCodecState);
    if (st_.finished) return 0;
    st_.finished = true;
    if (lib_->encoder_drain(enc_) != ope::kOk) return fail(kCodecFailed);
    return pull(true);
}

}  // namespace cassette
