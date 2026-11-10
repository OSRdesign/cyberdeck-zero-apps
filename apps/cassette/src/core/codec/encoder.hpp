/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: the encoder seam the writer drives (phase B). One encoder per take part, used by the writer thread
 * only:
 *   start(sink, tags)   the stream headers go out at once (FLAC: fLaC, STREAMINFO with "0 = unknown" totals, the Vorbis
 *                       comments, padding; Opus: the OpusHead and OpusTags pages), so an empty take is a valid file
 *   write(pcm, frames)  any block size, interleaved PCM in the capture format (16-bit or packed 24-bit little endian,
 *                       as the ring holds it); returns after the library took the frames
 *   flush()             pushes to the sink everything the library lets out without ending the stream; afterwards
 *                       stats().frames_out frames are in the sink, so after the writer's fdatasync a power cut keeps
 *                       that prefix (FLAC: complete frames, a block of 4096 frames at level 5; Opus: complete Ogg pages,
 *                       but libopusenc keeps up to its decision delay, 2 s by default, unencoded)
 *   finish()            ends the stream: the buffered rest is encoded; FLAC seeks back and rewrites STREAMINFO (total
 *                       samples, MD5, frame sizes); Opus writes the end-of-stream page. No write() afterwards.
 * Errors: the first error sticks; every later call returns it. The sink's -errno values pass through (ENOSPC, EIO).
 * Metadata: Vorbis comments (D16), the same names for FLAC and Opus.
 */
#pragma once

#include "../frame_sink.hpp"
#include "byte_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cassette {

/* A Vorbis comment NAME=value. Names: printable ASCII 0x20..0x7D without '=' (Vorbis I, section 5.2.3); upper case by
 * convention (TITLE, DATE, COMMENT, ENCODER, plus free fields such as SCENE, TAKE). Values: UTF-8. */
struct Tag {
    std::string name, value;
};
using Tags = std::vector<Tag>;
bool valid_tag_name(const std::string &name);

struct EncoderStats {
    uint64_t frames_in = 0;  /* frames given to write() */
    uint64_t frames_out = 0; /* frames whose encoded data is in the sink (FLAC: whole frames; Opus: from the granule
                                position of the last page written, converted to the input rate) */
    uint64_t bytes_out = 0;  /* bytes written to the sink, headers included (a STREAMINFO rewrite counts again) */
    uint64_t units_out = 0;  /* FLAC frames / Ogg pages written, headers included */
    bool finished = false;
    int error = 0;           /* the first error, 0 while all is well */
    /* Frames lost if the power went now (after the writer's next fdatasync): given but not yet in the sink. */
    uint64_t pending_frames() const { return frames_in > frames_out ? frames_in - frames_out : 0; }
};

class Encoder {
public:
    virtual ~Encoder() = default;
    virtual int start(ByteSink &sink, const Tags &tags) = 0;
    virtual int write(const void *pcm, size_t frames) = 0;
    virtual int flush() = 0;
    virtual int finish() = 0;
    virtual EncoderStats stats() const = 0;
    virtual const AudioFormat &format() const = 0;
};

/* Sample conversion from the capture format (16-bit or packed 24-bit little endian, interleaved). `samples` counts
 * samples (frames x channels). Byte-wise reads: the input needs no alignment. */
void pcm_to_int32(const void *in, unsigned bits, int32_t *out, size_t samples);  /* sign-extended, bits kept */
void pcm_to_float(const void *in, unsigned bits, float *out, size_t samples);    /* -1 .. +1 */
void pcm_to_int16(const void *in, unsigned bits, int16_t *out, size_t samples);  /* 24-bit: the top 16 bits */

}  // namespace cassette
