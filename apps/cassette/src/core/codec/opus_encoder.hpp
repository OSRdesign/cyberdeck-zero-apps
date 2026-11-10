/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: Ogg Opus through libopusenc's pull API (the encoder queues Ogg pages, Cassette writes them to a
 * ByteSink and can close the page being filled at any time).
 *   - 20 ms frames, mapping family 0 (mono or stereo), VBR, the bitrate and the application (speech: OPUS_APPLICATION_VOIP
 *     + OPUS_SIGNAL_VOICE; music: OPUS_APPLICATION_AUDIO + OPUS_SIGNAL_MUSIC) of the chosen preset
 *   - speech presets on a stereo card: OPUS_SET_FORCE_CHANNELS(1), the encoder codes one channel (no conversion here);
 *     the stream still says 2 channels
 *   - input rates 8, 12, 16, 24 or 48 kHz only (D11: no resampling in 0.1; note libopusenc itself always codes at
 *     48 kHz and upsamples any other input rate internally)
 *   - tags as OpusTags (Vorbis comments), 512 bytes of comment padding (libopusenc's default) for in-place edits
 * Delays (libopusenc's, kept by default): the decision delay holds up to 2 s of input unencoded (96000 samples at
 * 48 kHz); the muxing delay closes a page at least every 1 s of audio. So after flush() the sink holds every encoded
 * packet (the page being filled is closed), but up to decision_delay_ms of the newest input is still inside libopusenc:
 * a power cut loses the sync interval plus up to 2 s. finish() encodes that rest (a few hundred ms of CPU on a
 * Pi Zero 2 W for 2 s of audio, P0-e: 10-13x real time) and writes the end-of-stream page.
 */
#pragma once

#include "codec_libs.hpp"
#include "encoder.hpp"

#include <vector>

namespace cassette {

enum class OpusApplication { Speech, Music };

struct OpusConfig {
    AudioFormat fmt;
    int bitrate_bps = 32000;
    OpusApplication application = OpusApplication::Speech;
    bool force_mono = false;            /* code a stereo input as one channel */
    int complexity = 10;                /* 0..10; libopus' default is 10 (the P0-e timings) */
    bool vbr = true;
    uint32_t decision_delay_ms = 2000;  /* libopusenc's default; lower = a tighter crash-safe prefix */
    uint32_t muxing_delay_ms = 1000;    /* libopusenc's default: the longest page */
    int comment_padding = 512;          /* bytes */
    long serialno = -1;                 /* -1: libopusenc's random; tests set it for byte-exact output */
};

/* The rates the Opus coder works at natively. */
bool opus_rate_ok(uint32_t rate);
/* Can the Opus encoder take this capture format (an Opus rate, 1 or 2 channels, 16 or 24 bits)? false with a reason. */
bool opus_format_ok(const AudioFormat &fmt, std::string *why = nullptr);

class OpusEncoder final : public Encoder {
public:
    OpusEncoder(const OpusEncLib *lib, const OpusConfig &cfg);
    ~OpusEncoder() override;
    OpusEncoder(const OpusEncoder &) = delete;
    OpusEncoder &operator=(const OpusEncoder &) = delete;

    int start(ByteSink &sink, const Tags &tags) override;
    int write(const void *pcm, size_t frames) override;
    int flush() override;
    int finish() override;
    EncoderStats stats() const override { return st_; }
    const AudioFormat &format() const override { return cfg_.fmt; }

    uint32_t preskip() const { return preskip_; }        /* from OpusHead, 48 kHz samples */
    uint64_t last_granule() const { return granule_; }    /* of the last page written, 48 kHz incl. pre-skip */
    bool eos_written() const { return eos_; }

    static constexpr size_t kChunkFrames = 4800; /* frames converted per libopusenc call */

private:
    int pull(bool flush);
    int on_page(const unsigned char *page, size_t len);
    int fail(int err);

    const OpusEncLib *lib_;
    OpusConfig cfg_;
    ope::Comments *comments_ = nullptr;
    ope::Enc *enc_ = nullptr;
    ByteSink *sink_ = nullptr;
    EncoderStats st_;
    bool started_ = false;
    uint32_t preskip_ = 0;
    bool have_head_ = false;
    uint64_t granule_ = 0;
    bool eos_ = false;
    std::vector<int16_t> s16_;
    std::vector<float> f32_;
};

}  // namespace cassette
