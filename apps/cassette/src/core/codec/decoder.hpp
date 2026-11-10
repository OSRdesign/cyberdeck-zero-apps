/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: the DECODER seam, designed in phase A, implemented with playback of FLAC/Opus takes (phase C).
 * Nothing implements it yet and nothing calls it; tests/codec only checks that it compiles.
 *
 * Who will use it: the player (core/playback, today a WAV reader), the peak scanner (core/edit/peaks), trim/split
 * (core/edit/trim_split) and the repair of a cut FLAC/Opus take (core/repair, which needs the frame count of the
 * readable prefix). All of them read through FileOps (pread) like the WAV code does, so the fault-injecting test FileOps
 * keep working.
 *
 * Planned implementations:
 *   WavDecoder   the existing WAV/BWF/RF64 parser (wav.hpp) behind this interface; exact seeks
 *   FlacDecoder  libFLAC's stream decoder (FLAC__stream_decoder_init_stream with read/seek/tell/length/eof callbacks
 *                over FileOps), the same libFLAC.so.14/.12 already loaded for recording; seek_absolute is sample exact;
 *                a file whose STREAMINFO says 0 samples (cut by a power loss) decodes to the last whole frame
 *   OpusDecoder  libopusfile (libopusfile.so.0, package libopusfile0, needs libogg0) through op_open_callbacks over
 *                FileOps; output always 48 kHz (Opus decodes at 48 kHz: a 16 kHz take plays at 48 kHz, so the output
 *                device opens at 48 kHz or the player resamples); op_pcm_seek is sample exact via pre-skip; a stream
 *                without its end-of-stream page plays to the last complete page (RFC 7845: decoders must cope)
 * Loading follows codec_libs.hpp: a function table filled by dlopen, a fake table for the tests.
 */
#pragma once

#include "../fileops.hpp"
#include "../frame_sink.hpp"
#include "encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cassette {

enum class DecodedKind { Wav, Flac, OggOpus };

struct DecodedInfo {
    DecodedKind kind = DecodedKind::Wav;
    AudioFormat source;          /* as recorded (Opus: the input rate from OpusHead, informative only) */
    AudioFormat output;          /* what read() delivers: 16 or 24 bits as recorded for WAV/FLAC; Opus: 48 kHz, 16 bit */
    uint64_t total_frames = 0;   /* at the output rate; 0 with frames_known false when the header does not say */
    bool frames_known = false;   /* false for a cut FLAC (STREAMINFO 0) until scan_length() */
    Tags tags;                   /* Vorbis comments (WAV: from bext/iXML later) */
    bool damaged = false;        /* the end is missing or a frame failed its CRC: plays up to the damage */
};

class Decoder {
public:
    virtual ~Decoder() = default;
    /* Opens and reads the headers. 0, -errno, kCodecMissing (no library for this format) or kCodecFailed (not a
     * stream this decoder understands). */
    virtual int open(FileOps &ops, const std::string &path, DecodedInfo &info) = 0;
    /* Up to `frames` interleaved frames in info.output's format into `out`; the number read, 0 at the end, or < 0. */
    virtual long read(void *out, size_t frames) = 0;
    /* Positions the next read at `frame` (output rate). 0 or < 0. */
    virtual int seek(uint64_t frame) = 0;
    /* Decodes to the end to count the frames of a stream whose header does not say (repair, a cut FLAC). */
    virtual int scan_length(uint64_t &frames) = 0;
    virtual void close() = 0;
};

/* The factory phase C adds: picks the decoder by the file's magic ("RIFF"/"RF64", "fLaC", "OggS" + "OpusHead"), not by
 * the extension.
 *   std::unique_ptr<Decoder> make_decoder(FileOps &ops, const std::string &path, int *err);
 */

}  // namespace cassette
