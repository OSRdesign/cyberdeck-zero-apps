/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829) for VHS (brief 3.1, 10); VHS changes are marked VHS.
 *
 * Vinyl decoders: one audio stream as interleaved PCM frames.
 *
 * VHS: reduced to the FFmpeg decoder (ffmpeg_decoder.cpp). Vinyl's MP3 (libmpg123), FLAC, Ogg Vorbis, Ogg Opus and WAV /
 * AIFF readers, their magic-byte probe, the ReplayGain tags and the browser's name tests are not VHS's: a scene is an MKV
 * whose audio is AAC (or a foreign MP4 / MKV / MOV a person copied in), and libavformat opens all of those.
 *
 * Output: 16-bit samples (int16) for sources of 16 bits or fewer and the lossy formats (AAC), 32-bit samples (int32, the
 * source bits at the top) for 24-bit and 32-bit lossless sources, at the source rate, at most 8 channels (the engine plays
 * the first two). A seek lands on the packet at or before the frame asked for (av_seek_frame backward). LVGL-free.
 */
#pragma once

#include "decode_libs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vhs {

constexpr int kNeedsFfmpeg = -100300;  /* FFmpeg is not installed (or its libraries could not be mapped) */
constexpr int kNotAudio = -100302;     /* not a file libavformat opens */
constexpr int kDecodeFailed = -100303; /* the library failed on the file */
constexpr int kNoAudioStream = -100304; /* VHS: the file opens and has no audio stream (a video-only scene): not an error */

enum class Codec { Unknown, Ffmpeg };
const char *codec_name(Codec c); /* "FFmpeg" */

/* What read() delivers. */
struct PcmFormat {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 16; /* 16 (int16 samples) or 32 (int32 samples) */
    bool operator==(const PcmFormat &o) const { return rate == o.rate && channels == o.channels && bits == o.bits; }
    bool operator!=(const PcmFormat &o) const { return !(*this == o); }
    unsigned frame_bytes() const { return channels * (bits / 8u); }
};

struct TrackInfo {
    Codec codec = Codec::Unknown;
    std::string label;        /* the format's short name for the screen: "AAC", "ALAC" ... */
    PcmFormat out;
    unsigned source_bits = 0; /* 16, 24, 32 for lossless and PCM sources; 0 for the lossy formats */
    uint64_t total_frames = 0;
    bool frames_known = false;
    bool lossy = false;
    unsigned bitrate_kbps = 0; /* lossy formats when known */
};

class TrackDecoder {
public:
    virtual ~TrackDecoder() = default;
    virtual int open(const std::string &path, TrackInfo &info) = 0;
    /* Up to `frames` frames into out (frames * info.out.frame_bytes() bytes): frames read, 0 at the end, or < 0. */
    virtual long read(void *out, size_t frames) = 0;
    virtual int seek(uint64_t frame) = 0;
    virtual uint64_t position() const = 0;
    virtual void close() = 0;
};

/* The libraries the decoder uses; the getter is called only when a file is opened (it maps FFmpeg). A getter that
 * returns null means "not installed". */
struct DecodeLibs {
    std::function<const FfmpegLib *()> ffmpeg;
};
DecodeLibs real_decode_libs();

/* Opens the FFmpeg decoder for the file. Null with *err set (kNeedsFfmpeg, kNotAudio, kNoAudioStream, kDecodeFailed or
 * -errno). */
std::unique_ptr<TrackDecoder> open_track(const std::string &path, const DecodeLibs &libs, TrackInfo &info, int *err);

/* The sentence a notice shows for an open error ("The FFmpeg libraries could not be loaded. Reinstall VHS." for
 * kNeedsFfmpeg, "The file is gone ..." for -ENOENT). */
std::string track_error_text(int err, const std::string &path);

std::unique_ptr<TrackDecoder> make_ffmpeg_decoder(const FfmpegLib *lib);

/* The codec label of an FFmpeg decoder name ("aac" -> "AAC", "alac" -> "ALAC", "wmav2" -> "WMA", "mp3float" -> "MP3"). */
std::string ffmpeg_codec_label(const std::string &decoder_name);

}  // namespace vhs
