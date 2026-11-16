/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829) for VHS (brief 3.1, 10); VHS changes are marked VHS.
 *
 * Vinyl decoders: the function table of FFmpeg (libavformat, libavcodec, libavutil, libswresample), filled with
 * dlopen/dlsym at the first use. No FFmpeg header is needed to build: the few types and constants used are declared here
 * by hand, and tests/abi/check_decode_abi.cpp compares them with the real headers (tests/abi/run.sh, every root). The
 * tests fill the same table with fakes.
 *
 * VHS: only the FFmpeg part of Vinyl's file is kept (the tier-1 libraries libmpg123 and libvorbisfile, and Vinyl's
 * libFLAC / libopus helpers, are not VHS's: a scene's audio is AAC). The nested namespace is dav, not Vinyl's av:
 * record/av_libs.hpp already has vhs::av (the recorder's own hand-declared FFmpeg: mux, AAC encode, repair).
 * This table decodes audio for playback (libswresample turns the frames into interleaved int16), av_libs.hpp's writes.
 * Both dlopen the same libraries (one set each), loaded lazily: FFmpeg is mapped at the first playback, never at start
 * (+22.5 MB of memory on a Pi Zero 2 W, brief 3.3 rule 4).
 *
 * Library names tried, in order, as one matching set: libavformat.so.61 + libavcodec.so.61 + libavutil.so.59 +
 * libswresample.so.5 (FFmpeg 7.x, Debian 13), else the .62 / .62 / .60 / .6 set (FFmpeg 8.x, Ubuntu 26.04), else
 * .60 / .60 / .58 / .4 (FFmpeg 6.1, the build box's Ubuntu 24.04: tests only).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vhs {

/* ---------------------------------------------------------------------------------------------------------- FFmpeg */
namespace dav {
struct FormatContext;
struct CodecContext;
struct Codec;
struct Packet;
struct Frame;
struct Swr;
struct Dictionary;
constexpr int kMediaAudio = 1;
constexpr int kErrEagain = -11;
constexpr int kErrEof = -541478725; /* AVERROR_EOF = -FFERRTAG('E','O','F',' ') */
constexpr int kErrStreamNotFound = -1381258232; /* VHS: AVERROR_STREAM_NOT_FOUND (av_find_best_stream: no such stream) */
constexpr int kSeekBackward = 1;
constexpr int kSearchChildren = 1;
constexpr int kLogQuiet = -8;
constexpr int64_t kNoPts = INT64_MIN;
enum SampleFmt { kU8 = 0, kS16 = 1, kS32 = 2, kFlt = 3, kDbl = 4, kU8P = 5, kS16P = 6, kS32P = 7, kFltP = 8, kDblP = 9,
                 kS64 = 10, kS64P = 11 };
struct ChannelLayout { /* AVChannelLayout (FFmpeg 5.1 and later) */
    int order;
    int nb_channels;
    uint64_t mask;
    void *opaque;
};
struct Rational {
    int num, den;
};
/* The struct fields VHS reads, by libavformat major version (check_decode_abi.cpp checks them against the headers). */
struct Offsets {
    size_t fmt_nb_streams = 44, fmt_streams = 48, fmt_start_time = 96, fmt_duration = 104; /* AVFormatContext (VHS: start, duration) */
    size_t stream_codecpar = 16, stream_time_base = 32, stream_duration = 48; /* AVStream */
    size_t packet_pts = 8, packet_stream_index = 36;                          /* AVPacket (VHS: pts) */
    size_t frame_extended_data = 96, frame_nb_samples = 112, frame_format = 116; /* AVFrame */
};
Offsets offsets_for(int lavf_major);

/* The library sets tried, in order (one soname each; tests/abi checks them against the headers' majors). */
struct LibSet {
    const char *fmt, *codec, *util, *swr;
    int major;
};
constexpr int kLibSetCount = 3;
extern const LibSet kLibSets[kLibSetCount];
}  // namespace dav

struct FfmpegLib {
    std::string name;     /* "libavformat.so.61 ..." */
    int lavf_major = 0;
    dav::Offsets off;
    /* libavutil */
    unsigned (*avutil_version)() = nullptr;
    void (*log_set_level)(int) = nullptr;
    dav::Frame *(*frame_alloc)() = nullptr;
    void (*frame_free)(dav::Frame **) = nullptr;
    void (*frame_unref)(dav::Frame *) = nullptr;
    int (*opt_get_int)(void *obj, const char *name, int flags, int64_t *out) = nullptr;
    int (*opt_get_sample_fmt)(void *obj, const char *name, int flags, int *out) = nullptr;
    int (*opt_get_chlayout)(void *obj, const char *name, int flags, dav::ChannelLayout *out) = nullptr;
    void (*chlayout_default)(dav::ChannelLayout *, int nb_channels) = nullptr;
    void (*chlayout_uninit)(dav::ChannelLayout *) = nullptr;
    /* libavformat */
    unsigned (*avformat_version)() = nullptr;
    int (*open_input)(dav::FormatContext **, const char *url, const void *fmt, dav::Dictionary **opts) = nullptr;
    int (*find_stream_info)(dav::FormatContext *, dav::Dictionary **opts) = nullptr;
    int (*find_best_stream)(dav::FormatContext *, int type, int wanted, int related, const dav::Codec **dec,
                            int flags) = nullptr;
    void (*close_input)(dav::FormatContext **) = nullptr;
    int (*read_frame)(dav::FormatContext *, dav::Packet *) = nullptr;
    int (*seek_frame)(dav::FormatContext *, int stream, int64_t ts, int flags) = nullptr;
    /* libavcodec */
    dav::CodecContext *(*alloc_context)(const dav::Codec *) = nullptr;
    void (*free_context)(dav::CodecContext **) = nullptr;
    int (*parameters_to_context)(dav::CodecContext *, const void *par) = nullptr;
    int (*open2)(dav::CodecContext *, const dav::Codec *, dav::Dictionary **) = nullptr;
    int (*send_packet)(dav::CodecContext *, const dav::Packet *) = nullptr;
    int (*receive_frame)(dav::CodecContext *, dav::Frame *) = nullptr;
    void (*flush_buffers)(dav::CodecContext *) = nullptr;
    dav::Packet *(*packet_alloc)() = nullptr;
    void (*packet_free)(dav::Packet **) = nullptr;
    void (*packet_unref)(dav::Packet *) = nullptr;
    /* libswresample */
    int (*swr_alloc_set_opts2)(dav::Swr **, const dav::ChannelLayout *out_layout, int out_fmt, int out_rate,
                               const dav::ChannelLayout *in_layout, int in_fmt, int in_rate, int log_offset,
                               void *log_ctx) = nullptr;
    int (*swr_init)(dav::Swr *) = nullptr;
    void (*swr_free)(dav::Swr **) = nullptr;
    int (*swr_convert)(dav::Swr *, uint8_t **out, int out_count, const uint8_t **in, int in_count) = nullptr;
    bool complete() const;
};

/* Loaded on the first call, then cached for the life of the process (never unloaded). Null with `why` when missing.
 * Thread-safe. real_ffmpeg() is the only call that maps FFmpeg for playback. */
const FfmpegLib *real_ffmpeg(std::string *why = nullptr);
/* True once real_ffmpeg() mapped the libraries (for the log and the memory note). */
bool ffmpeg_loaded();

/* The package names a notice gives for a missing library. */
constexpr const char *kFfmpegPackages = "libavformat61 libavcodec61 libswresample5";

}  // namespace vhs
