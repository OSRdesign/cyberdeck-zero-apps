/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: Ogg Opus through libopus' decoder over Cassette's own Ogg reader (decoder.hpp, ogg_opus.hpp).
 * Output: 16-bit interleaved at oggopus::decode_rate(input rate). A seek resets the decoder, finds the page by bisection,
 * decodes 80 ms of pre-roll before the target and drops it, so the first frame after a seek is the same sample a linear
 * decode gives, give or take the decoder's convergence (RFC 7845 section 4.6). A packet that does not decode is
 * concealed (libopus' packet loss concealment) with its own duration, so every later sample keeps its place.
 */
#pragma once

#include "decoder.hpp"
#include "ogg_opus.hpp"

#include <memory>
#include <vector>

namespace cassette {

class OpusFileDecoder final : public Decoder {
public:
    explicit OpusFileDecoder(const OpusDecLib *lib) : lib_(lib) {}
    ~OpusFileDecoder() override { close(); }
    OpusFileDecoder(const OpusFileDecoder &) = delete;
    OpusFileDecoder &operator=(const OpusFileDecoder &) = delete;

    int open(FileOps &ops, const std::string &path, DecodedInfo &info) override;
    long read(void *out, size_t frames) override;
    int seek(uint64_t frame) override;
    int seek_near(uint64_t frame, uint64_t &landed) override;
    uint64_t position() const override { return pos_; }
    int scan_length(uint64_t &frames) override {
        frames = total_;
        return 0;
    }
    void close() override;

    unsigned rate_k() const { return tl_.k; }
    oggopus::OggOpusFile *file() { return file_.get(); }

    struct Stats {
        uint64_t packets = 0;
        uint64_t frames_decoded = 0; /* at the output rate, pre-roll and pre-skip included */
        uint64_t concealed = 0;      /* packets that did not decode */
        uint64_t seeks = 0;
    };
    Stats stats() const { return st_; }

private:
    int decode_next(); /* one packet: 1, 0 at the end, < 0 */
    void reset_buffers(uint64_t frame);

    const OpusDecLib *lib_;
    std::unique_ptr<oggopus::OggOpusFile> file_;
    opus::Decoder *dec_ = nullptr;
    oggopus::Timeline tl_;
    unsigned ch_ = 1;
    uint32_t rate_ = 48000;
    uint64_t total_ = 0;
    uint64_t pos_ = 0;       /* the frame read() returns next */
    uint64_t want_ = 0;      /* decoded frames before this are dropped */
    uint64_t next_out_ = 0;  /* the frame the next kept sample is (pcm_ holds [pos_, next_out_)) */
    std::vector<int16_t> pcm_;
    size_t pcm_off_ = 0;     /* frames of pcm_ already returned */
    std::vector<int16_t> tmp_;
    oggopus::Packet pending_;
    bool have_pending_ = false;
    bool eos_ = false;
    Stats st_;
};

}  // namespace cassette
