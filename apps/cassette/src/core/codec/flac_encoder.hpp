/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: FLAC through libFLAC's stream encoder (write, seek and tell callbacks into a ByteSink).
 *   - streamable subset (decoders can start at any frame; what makes a cut file playable and repairable)
 *   - compression level 5 by default (P0-e on the zero8: 27 % of PCM on mono speech at -5, 28 % at -0, 171x real time)
 *   - 16-bit or packed 24-bit input, 1 or 2 channels (libFLAC takes up to 8; Cassette records at most 2)
 *   - Vorbis comments from the take's tags, then a PADDING block so tags can be edited in place later
 *   - STREAMINFO is written with total samples 0 and MD5 0 ("unknown") and rewritten by finish(); a file cut by a power
 *     loss keeps every frame written before the last fdatasync and plays to its end; repair (phase B) counts the frames
 *     and rewrites STREAMINFO in place
 * libFLAC writes a frame as soon as a block (4096 samples per channel at level 5) is complete, during write(); a partial
 * block stays inside libFLAC until finish(), so flush() has nothing more to push.
 */
#pragma once

#include "codec_libs.hpp"
#include "encoder.hpp"

#include <vector>

namespace cassette {

struct FlacConfig {
    AudioFormat fmt;
    unsigned level = 5;            /* 0..8 */
    bool verify = false;           /* libFLAC's verify decoder: doubles the CPU, off */
    uint32_t padding_bytes = 4096; /* PADDING block after the comments; 0: none */
};

/* Can the FLAC encoder take this capture format (16 or 24 bits, 1 or 2 channels, a rate libFLAC's subset allows)?
 * false with a short reason. */
bool flac_format_ok(const AudioFormat &fmt, std::string *why = nullptr);

class FlacEncoder final : public Encoder {
public:
    /* `lib` may be null (the library is missing): start() then returns kCodecMissing. */
    FlacEncoder(const FlacLib *lib, const FlacConfig &cfg);
    ~FlacEncoder() override;
    FlacEncoder(const FlacEncoder &) = delete;
    FlacEncoder &operator=(const FlacEncoder &) = delete;

    int start(ByteSink &sink, const Tags &tags) override;
    int write(const void *pcm, size_t frames) override;
    int flush() override;
    int finish() override;
    EncoderStats stats() const override { return st_; }
    const AudioFormat &format() const override { return cfg_.fmt; }

    static constexpr size_t kChunkFrames = 4096; /* frames converted per process_interleaved call */

private:
    static int write_cb(const flac::Encoder *, const uint8_t *buf, size_t bytes, uint32_t samples, uint32_t frame,
                        void *client);
    static int seek_cb(const flac::Encoder *, uint64_t off, void *client);
    static int tell_cb(const flac::Encoder *, uint64_t *off, void *client);
    int fail(int err);
    int lib_error(); /* the sink's error when there is one, else kCodecFailed */

    const FlacLib *lib_;
    FlacConfig cfg_;
    flac::Encoder *enc_ = nullptr;
    std::vector<flac::Metadata *> meta_;
    ByteSink *sink_ = nullptr;
    EncoderStats st_;
    int sink_err_ = 0;
    bool started_ = false;
    bool detached_ = false; /* the destructor runs: callbacks must not touch the sink */
    std::vector<int32_t> scratch_;
};

}  // namespace cassette
