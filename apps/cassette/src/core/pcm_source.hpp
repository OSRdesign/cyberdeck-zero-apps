/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette core: a random-access reader of a take's audio as interleaved little-endian PCM frames. The Player and the
 * peak cache read through it: WavReader (playback/player.hpp) reads a WAV file's bytes, DecodedSource
 * (codec/decoded_source.hpp) decodes a FLAC or Ogg Opus take. LVGL-free.
 */
#pragma once

#include "frame_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cassette {

class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual int open(const std::string &path) = 0; /* 0 or < 0 */
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual const AudioFormat &format() const = 0; /* what read() delivers */
    virtual uint32_t block_align() const = 0;      /* bytes per frame */
    virtual uint64_t frames() const = 0;
    /* Up to n frames from `frame` into buf (n * block_align bytes): frames read (0 at the end) or < 0. */
    virtual long read(uint64_t frame, size_t n, void *buf) = 0;
};

}  // namespace cassette
