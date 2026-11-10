/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: the DECODER seam (designed in phase A, implemented in phase C: playback, preview, the peak cache and
 * trim of FLAC and Ogg Opus takes). WAV keeps its own reader (playback/player.hpp WavReader), its own peak builder and
 * its byte-copy trim; FLAC and Opus go through a Decoder, and DecodedSource (decoded_source.hpp) gives the Player and the
 * peak cache the same random-access frame reader WAV has.
 *
 * Implementations (everything through FileOps, so the fault-injecting test FileOps keep working):
 *   FlacDecoder      libFLAC's stream decoder (FLAC__stream_decoder_init_stream with read/seek/tell/length/eof callbacks
 *                    over FileOps), the same libFLAC.so.14/.12 recording loads. Output: the recorded format (16 or 24
 *                    bits, packed little endian, as WAV). Seeks are sample exact (FLAC__stream_decoder_seek_absolute:
 *                    libFLAC's own bisection over the frames, a few small reads; no index of the whole file). A stream
 *                    whose STREAMINFO says 0 samples (a take still recording, or a repair that failed) gets its length
 *                    from the frame scan of codec/stream_scan.
 *   OpusFileDecoder  libopus' decoder (libopus.so.0) over Cassette's own Ogg reader (codec/ogg_opus.*: no libopusfile,
 *                    no libogg). Output: 16 bits at the take's input rate when that is an Opus rate (8, 12, 16, 24 or
 *                    48 kHz: a take plays at the rate it was recorded, no resampling), else 48 kHz. Seeks are sample
 *                    exact: bisection over the pages by granule position, 80 ms of pre-roll decoded and dropped, the
 *                    pre-skip honoured, the end trimming of the last page honoured.
 * make_decoder() picks one by the file's magic ("fLaC", "OggS" + OpusHead), not by the extension.
 */
#pragma once

#include "../fileops.hpp"
#include "../frame_sink.hpp"
#include "codec_libs.hpp"
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
    AudioFormat output;          /* what read() delivers: 16 or 24 bits as recorded for FLAC; Opus: 16 bits (see above) */
    uint64_t total_frames = 0;   /* at the output rate */
    bool frames_known = false;   /* false when the header did not say and the length came from a scan */
    Tags tags;                   /* Vorbis comments */
    std::string vendor;          /* the comment block's vendor string */
    bool damaged = false;        /* the end is missing (no end-of-stream page, a cut frame): plays up to the damage */
};

class Decoder {
public:
    virtual ~Decoder() = default;
    /* Opens and reads the headers. 0, -errno, kCodecMissing (no library for this format) or kCodecBadStream. */
    virtual int open(FileOps &ops, const std::string &path, DecodedInfo &info) = 0;
    /* Up to `frames` interleaved frames in info.output's format into `out`; the number read, 0 at the end, or < 0. */
    virtual long read(void *out, size_t frames) = 0;
    /* Positions the next read at `frame` (output rate), sample exact. 0 or < 0. */
    virtual int seek(uint64_t frame) = 0;
    /* Positions the next read at a frame `landed` <= `frame` from which reading costs no more than seek(frame) would
     * (Opus: the start of the converged part after the page and the pre-roll a seek decodes anyway; FLAC: `frame`).
     * DecodedSource keeps those frames for the reverse scan. 0 or < 0. */
    virtual int seek_near(uint64_t frame, uint64_t &landed) {
        landed = frame;
        return seek(frame);
    }
    /* The frame the next read starts at. */
    virtual uint64_t position() const = 0;
    /* Decodes to the end to count the frames (a stream whose header does not say). Leaves the position at the end. */
    virtual int scan_length(uint64_t &frames) = 0;
    virtual void close() = 0;
};

/* Opens a decoder for the file at `path`, chosen by its magic: FlacDecoder for "fLaC", OpusFileDecoder for an Ogg
 * stream (OpusHead is checked by its open()). Null with *err set: kCodecBadStream for anything else (WAV included: it
 * has its own reader), kCodecMissing when the library for the format is not in `libs`, -errno when the file cannot be
 * read. */
std::unique_ptr<Decoder> make_decoder(FileOps &ops, const std::string &path, const DecoderLibs &libs, DecodedInfo &info,
                                      int *err);

/* Is this file one a Decoder plays (FLAC or Ogg Opus)? By the extension, as the take list decides (.flac, .opus); by the
 * magic when the name has none of the take extensions. */
bool is_compressed_take(FileOps &ops, const std::string &path);

/* The text a notice shows when a take cannot be decoded: "FLAC not installed (libflac14)", "Opus not installed
 * (libopus0)", the codec error, or strerror. The format comes from the file name's extension. */
std::string decode_error_text(int err, const std::string &path);

}  // namespace cassette
