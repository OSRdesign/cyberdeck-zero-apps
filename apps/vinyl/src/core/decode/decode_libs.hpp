/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl decoders: the function tables of the small tier-1 libraries (libmpg123, libvorbisfile) and of FFmpeg (tier 2:
 * libavformat, libavcodec, libavutil, libswresample), filled with dlopen/dlsym at the first use, as Cassette loads libFLAC
 * and libopus (codec/codec_libs.hpp, which Vinyl keeps for FLAC and Opus). No codec header is needed to build: the few
 * types and constants used are declared here by hand, and tests/check_decode_abi.cpp compares them with the real headers
 * where they are installed. The tests fill the same tables with fakes.
 *
 * Report 059, decisions D2 and D3: the tier-1 libraries are package Depends (loaded on the first file of their format);
 * FFmpeg is a Recommends and is loaded only when a file needs it (never at start-up: +22.5 MB of memory on a Pi Zero 2 W).
 *
 * Library names tried, in order:
 *   MP3     libmpg123.so.0 (package libmpg123-0t64)
 *   Vorbis  libvorbisfile.so.3 (libvorbisfile3, which pulls libvorbis0a and libogg0)
 *   FFmpeg  one matching set: libavformat.so.61 + libavcodec.so.61 + libavutil.so.59 + libswresample.so.5 (FFmpeg 7.x,
 *           Debian 13), else the .62 / .62 / .60 / .6 set (FFmpeg 8.x, Ubuntu 26.04), else .60 / .60 / .58 / .4 (FFmpeg
 *           6.1, the build box's Ubuntu 24.04: tests only).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vinyl {

/* ---------------------------------------------------------------------------------------------------------- mpg123 */
namespace mpg {
struct Handle;
constexpr int kOk = 0, kDone = -12, kNewFormat = -11, kNeedMore = -10, kErr = -1;
constexpr long kParamFlags = 1, kParamAddFlags = 2, kParamRemoveFlags = 13;
constexpr long kFlagQuiet = 0x20, kFlagGapless = 0x40, kFlagForceStereo = 0x8;
constexpr int kMono = 1, kStereo = 2;
constexpr int kEncSigned16 = 0xd0;
}  // namespace mpg

struct Mpg123Lib {
    std::string name;
    int (*init)() = nullptr; /* optional (a no-op in 1.32) */
    mpg::Handle *(*create)(const char *decoder, int *error) = nullptr;            /* mpg123_new */
    void (*destroy)(mpg::Handle *) = nullptr;                                    /* mpg123_delete */
    int (*param)(mpg::Handle *, long type, long value, double fvalue) = nullptr; /* mpg123_param */
    int (*format_none)(mpg::Handle *) = nullptr;
    int (*format)(mpg::Handle *, long rate, int channels, int encodings) = nullptr;
    int (*open)(mpg::Handle *, const char *path) = nullptr;
    int (*close)(mpg::Handle *) = nullptr;
    int (*getformat)(mpg::Handle *, long *rate, int *channels, int *encoding) = nullptr;
    int (*read)(mpg::Handle *, void *out, size_t size, size_t *done) = nullptr;
    int64_t (*seek)(mpg::Handle *, int64_t sample, int whence) = nullptr; /* off_t on the 64-bit boards */
    int64_t (*tell)(mpg::Handle *) = nullptr;
    int64_t (*length)(mpg::Handle *) = nullptr;
    int (*scan)(mpg::Handle *) = nullptr;
    const char *(*strerror)(mpg::Handle *) = nullptr;
    bool complete() const;
};

/* ------------------------------------------------------------------------------------------------------ vorbisfile */
namespace ov {
/* OggVorbis_File is about 950 bytes on a 64-bit build; Vinyl keeps a larger, aligned buffer for it (check_decode_abi
 * compares sizeof). */
constexpr size_t kFileBytes = 2048;
struct Callbacks {
    size_t (*read)(void *ptr, size_t size, size_t nmemb, void *src);
    int (*seek)(void *src, int64_t offset, int whence);
    int (*close)(void *src);
    long (*tell)(void *src);
};
/* the first fields of vorbis_info */
struct Info {
    int version;
    int channels;
    long rate;
};
}  // namespace ov

struct VorbisFileLib {
    std::string name;
    int (*open_callbacks)(void *src, void *vf, const char *initial, long ibytes, ov::Callbacks cb) = nullptr;
    int (*clear)(void *vf) = nullptr;
    ov::Info *(*info)(void *vf, int link) = nullptr;
    int64_t (*pcm_total)(void *vf, int link) = nullptr;
    long (*read)(void *vf, char *buf, int length, int bigendian, int word, int sgned, int *bitstream) = nullptr;
    int (*pcm_seek)(void *vf, int64_t pos) = nullptr;
    int64_t (*pcm_tell)(void *vf) = nullptr;
    long (*bitrate)(void *vf, int link) = nullptr;
    bool complete() const;
};

/* ---------------------------------------------------------------------------------------------------------- FFmpeg */
namespace av {
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
/* The struct fields Vinyl reads, by libavformat major version (check_decode_abi.cpp checks them against the headers). */
struct Offsets {
    size_t fmt_nb_streams = 44, fmt_streams = 48;                          /* AVFormatContext */
    size_t stream_codecpar = 16, stream_time_base = 32, stream_duration = 48; /* AVStream */
    size_t packet_stream_index = 36;                                       /* AVPacket */
    size_t frame_extended_data = 96, frame_nb_samples = 112, frame_format = 116; /* AVFrame */
};
Offsets offsets_for(int lavf_major);
}  // namespace av

struct FfmpegLib {
    std::string name;     /* "libavformat.so.61 ..." */
    int lavf_major = 0;
    av::Offsets off;
    /* libavutil */
    unsigned (*avutil_version)() = nullptr;
    void (*log_set_level)(int) = nullptr;
    av::Frame *(*frame_alloc)() = nullptr;
    void (*frame_free)(av::Frame **) = nullptr;
    void (*frame_unref)(av::Frame *) = nullptr;
    int (*opt_get_int)(void *obj, const char *name, int flags, int64_t *out) = nullptr;
    int (*opt_get_sample_fmt)(void *obj, const char *name, int flags, int *out) = nullptr;
    int (*opt_get_chlayout)(void *obj, const char *name, int flags, av::ChannelLayout *out) = nullptr;
    void (*chlayout_default)(av::ChannelLayout *, int nb_channels) = nullptr;
    void (*chlayout_uninit)(av::ChannelLayout *) = nullptr;
    /* libavformat */
    unsigned (*avformat_version)() = nullptr;
    int (*open_input)(av::FormatContext **, const char *url, const void *fmt, av::Dictionary **opts) = nullptr;
    int (*find_stream_info)(av::FormatContext *, av::Dictionary **opts) = nullptr;
    int (*find_best_stream)(av::FormatContext *, int type, int wanted, int related, const av::Codec **dec,
                            int flags) = nullptr;
    void (*close_input)(av::FormatContext **) = nullptr;
    int (*read_frame)(av::FormatContext *, av::Packet *) = nullptr;
    int (*seek_frame)(av::FormatContext *, int stream, int64_t ts, int flags) = nullptr;
    /* libavcodec */
    av::CodecContext *(*alloc_context)(const av::Codec *) = nullptr;
    void (*free_context)(av::CodecContext **) = nullptr;
    int (*parameters_to_context)(av::CodecContext *, const void *par) = nullptr;
    int (*open2)(av::CodecContext *, const av::Codec *, av::Dictionary **) = nullptr;
    int (*send_packet)(av::CodecContext *, const av::Packet *) = nullptr;
    int (*receive_frame)(av::CodecContext *, av::Frame *) = nullptr;
    void (*flush_buffers)(av::CodecContext *) = nullptr;
    av::Packet *(*packet_alloc)() = nullptr;
    void (*packet_free)(av::Packet **) = nullptr;
    void (*packet_unref)(av::Packet *) = nullptr;
    /* libswresample */
    int (*swr_alloc_set_opts2)(av::Swr **, const av::ChannelLayout *out_layout, int out_fmt, int out_rate,
                               const av::ChannelLayout *in_layout, int in_fmt, int in_rate, int log_offset,
                               void *log_ctx) = nullptr;
    int (*swr_init)(av::Swr *) = nullptr;
    void (*swr_free)(av::Swr **) = nullptr;
    int (*swr_convert)(av::Swr *, uint8_t **out, int out_count, const uint8_t **in, int in_count) = nullptr;
    bool complete() const;
};

/* Loaded on the first call, then cached for the life of the process (never unloaded). Null with `why` when missing.
 * Thread-safe. real_ffmpeg() is the only call that maps FFmpeg: nothing else of Vinyl touches it. */
const Mpg123Lib *real_mpg123(std::string *why = nullptr);
const VorbisFileLib *real_vorbisfile(std::string *why = nullptr);
const FfmpegLib *real_ffmpeg(std::string *why = nullptr);
/* True once real_ffmpeg() mapped the libraries (for the log and the memory note). */
bool ffmpeg_loaded();

/* The package names a notice gives for a missing library. */
constexpr const char *kMpg123Package = "libmpg123-0t64";
constexpr const char *kVorbisPackage = "libvorbisfile3";
constexpr const char *kFfmpegPackages = "libavformat61 libavcodec61 libswresample5";

}  // namespace vinyl
