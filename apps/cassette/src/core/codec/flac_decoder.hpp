/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: FLAC through libFLAC's stream decoder (decoder.hpp). The file is read with pread through FileOps in
 * the decoder's callbacks; MD5 checking is off (a seek turns it off anyway); positions come from the frame headers, so a
 * frame lost to damage in the middle becomes silence of its length and every later sample keeps its place.
 */
#pragma once

#include "decoder.hpp"

#include <vector>

namespace cassette {

/* The Vorbis comments (and the vendor string) of a FLAC file, from its metadata blocks; no library needed. false when
 * the header cannot be read (*err: -errno when a read failed). */
bool read_flac_tags(FileOps &ops, const std::string &path, std::string &vendor, Tags &tags, int *err = nullptr);

class FlacDecoder final : public Decoder {
public:
    explicit FlacDecoder(const FlacDecLib *lib) : lib_(lib) {}
    ~FlacDecoder() override { close(); }
    FlacDecoder(const FlacDecoder &) = delete;
    FlacDecoder &operator=(const FlacDecoder &) = delete;

    int open(FileOps &ops, const std::string &path, DecodedInfo &info) override;
    long read(void *out, size_t frames) override;
    int seek(uint64_t frame) override;
    uint64_t position() const override { return pos_; }
    int scan_length(uint64_t &frames) override;
    void close() override;

    struct Stats {
        uint64_t frames_decoded = 0; /* samples per channel the library handed over */
        uint64_t seeks = 0;
        uint64_t gaps = 0;           /* frames lost to damage, filled with silence */
    };
    Stats stats() const { return st_; }

private:
    static int read_cb(const flac::Decoder *, uint8_t buffer[], size_t *bytes, void *client);
    static int seek_cb(const flac::Decoder *, uint64_t off, void *client);
    static int tell_cb(const flac::Decoder *, uint64_t *off, void *client);
    static int length_cb(const flac::Decoder *, uint64_t *len, void *client);
    static flac::Bool eof_cb(const flac::Decoder *, void *client);
    static int write_cb(const flac::Decoder *, const flac::FrameHeader *frame, const int32_t *const buffer[],
                        void *client);
    static void error_cb(const flac::Decoder *, int status, void *client);
    int fill(); /* one more frame into pend_: 1, 0 at the end, < 0 */

    const FlacDecLib *lib_;
    FileOps *ops_ = nullptr;
    std::string path_;
    int fd_ = -1;
    uint64_t size_ = 0, fpos_ = 0;
    int io_err_ = 0;
    flac::Decoder *dec_ = nullptr;
    bool inited_ = false;
    AudioFormat src_, out_;
    unsigned out_bytes_ = 2;
    int shift_ = 0;
    uint32_t nominal_block_ = 0;
    uint64_t total_ = 0;
    uint64_t pos_ = 0;           /* the frame read() returns next */
    uint64_t expect_ = 0;        /* the frame the next decoded block must start at (pend_ ends there) */
    std::vector<int32_t> pend_;  /* decoded interleaved samples: frames [pos_, expect_) from pend_off_ on */
    size_t pend_off_ = 0;        /* frames of pend_ already returned */
    bool eos_ = false;
    bool damaged_ = false;
    Stats st_;
};

}  // namespace cassette
