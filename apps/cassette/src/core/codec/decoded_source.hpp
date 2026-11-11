/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: a FLAC or Ogg Opus take as a random-access PcmSource (pcm_source.hpp), for the Player and the peak
 * cache. It keeps one window of decoded frames:
 *   - reading on from the end of the window (normal play, the forward 5x scan) decodes just what is asked, in the
 *     caller's thread (the Player's playback thread: its 500 ms device buffer absorbs the decoding time);
 *   - a read just before the window (the reverse scan reads its blocks backwards) decodes a whole window that ends where
 *     the read ends (window_frames(), half a second, plus whatever the decoder had to decode anyway to get there: an
 *     Opus seek starts at a page and decodes 80 ms of pre-roll), so the next reverse blocks are already there;
 *   - any other read (a jump, a seek) repositions the decoder there.
 * The window stays bounded (a few window lengths). Not thread-safe: one user at a time.
 */
#pragma once

#include "../pcm_source.hpp"
#include "decoder.hpp"

#include <memory>
#include <vector>

namespace cassette {

class DecodedSource final : public PcmSource {
public:
    /* libs: the decoders to use; null = real_decoder_libs() (loaded at the first open). */
    DecodedSource(FileOps &ops, const DecoderLibs *libs = nullptr) : ops_(ops), libs_(libs) {}
    ~DecodedSource() override { close(); }
    DecodedSource(const DecodedSource &) = delete;
    DecodedSource &operator=(const DecodedSource &) = delete;

    int open(const std::string &path) override; /* 0, -errno, kCodecMissing, kCodecBadStream */
    void close() override;
    bool is_open() const override { return dec_ != nullptr; }
    const AudioFormat &format() const override { return info_.output; }
    uint32_t block_align() const override { return align_; }
    uint64_t frames() const override { return info_.total_frames; }
    long read(uint64_t frame, size_t n, void *buf) override;

    const DecodedInfo &info() const { return info_; }
    uint64_t window_frames() const;

    struct Stats {
        uint64_t requested = 0; /* frames asked for */
        uint64_t decoded = 0;   /* frames the decoder delivered into the window */
        uint64_t seeks = 0;     /* decoder repositionings */
        uint64_t reverse = 0;   /* of which reverse windows */
    };
    Stats stats() const { return st_; }
    Decoder *decoder() { return dec_.get(); }

private:
    int extend(uint64_t end); /* decodes on until the window reaches `end` (or the stream ends) */

    FileOps &ops_;
    const DecoderLibs *libs_;
    std::unique_ptr<Decoder> dec_;
    DecodedInfo info_;
    uint32_t align_ = 0;
    std::vector<unsigned char> cache_; /* frames [c0_, c0_ + cn_) */
    uint64_t c0_ = 0, cn_ = 0;
    uint64_t dpos_ = 0;                /* the decoder's next frame */
    Stats st_;
};

}  // namespace cassette
