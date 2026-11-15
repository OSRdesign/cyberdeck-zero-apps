/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: FFmpeg for recording (brief 3.1, 6.1, 6.3; Vinyl's decode_libs pattern): libavformat, libavcodec and libavutil
 * opened with dlopen at the first recording or repair (never at start: +22.5 MB on a Zero 2 W, brief 3.3 rule 4), one
 * matching set: .61 / .61 / .59 (FFmpeg 7.x, Debian 13), else .62 / .62 / .60 (FFmpeg 8.x, Ubuntu 26.04), else
 * .60 / .60 / .58 (FFmpeg 6.1, the build box: tests only). No FFmpeg header is needed to build: the types, the few
 * struct fields and the constants used are declared here by hand per libavformat major, and tests/abi/check_av_abi.cpp
 * compares them with the real headers of the 61 and 62 sets (and the build box's 60). Codec-context fields go through
 * AVOptions where they can (sample rate, channel layout, bit rate, flags); the time base and the sample format (it has
 * no AVOption), the stream and packet fields and the frame fields go through the offsets.
 * Used by the writer thread (mkv_writer) and the scenes worker (repair), each with its own contexts. LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vhs {
namespace av {

struct FormatContext;
struct OutputFormat;
struct InputFormat;
struct Stream;
struct CodecContext;
struct Codec;
struct CodecParameters;
struct Packet;
struct Frame;
struct Dictionary;
struct IOContext;

constexpr int kMediaVideo = 0, kMediaAudio = 1;
constexpr int kCodecMjpeg = 7;     /* AV_CODEC_ID_MJPEG */
constexpr int kCodecH264 = 27;     /* AV_CODEC_ID_H264 (P2) */
constexpr int kCodecAac = 86018;   /* AV_CODEC_ID_AAC */
constexpr int kCodecHevc = 173, kCodecMpeg4 = 12, kCodecVp8 = 139, kCodecVp9 = 167; /* the scenes list's codec names */
/* playback (M5, brief 4.4): the pixel formats the own YUV-to-RGB takes, the seek flag */
constexpr int kPixYuv420p = 0, kPixYuv422p = 4, kPixYuvj420p = 12, kPixYuvj422p = 13, kPixNv12 = 23;
constexpr int kSeekBackward = 1; /* AVSEEK_FLAG_BACKWARD */
constexpr int kIoWrite = 2;        /* AVIO_FLAG_WRITE */
constexpr int kPktKey = 1;         /* AV_PKT_FLAG_KEY */
constexpr int kFmtFltp = 8;        /* AV_SAMPLE_FMT_FLTP */
constexpr int kErrEagain = -11;    /* AVERROR(EAGAIN) */
constexpr int kErrEof = -541478725; /* AVERROR_EOF */
constexpr int kLogQuiet = -8, kLogError = 16;
constexpr std::int64_t kNoPts = INT64_MIN;

struct Rational {
    int num, den;
};
struct ChannelLayout { /* AVChannelLayout (FFmpeg 5.1 and later) */
    int order;
    int nb_channels;
    std::uint64_t mask;
    void *opaque;
};

/* The struct fields VHS reads or writes, by libavformat major (the same major for libavcodec in each set). */
struct Offsets {
    std::size_t fmt_pb = 32, fmt_nb_streams = 44, fmt_streams = 48, fmt_duration = 104; /* AVFormatContext */
    std::size_t stream_codecpar = 16, stream_time_base = 32, stream_avg_frame_rate = 88; /* AVStream */
    std::size_t par_codec_type = 0, par_codec_id = 4, par_width = 72, par_height = 76; /* AVCodecParameters */
    std::size_t ctx_time_base = 84, ctx_sample_fmt = 348;                      /* AVCodecContext (sample_fmt has no AVOption) */
    std::size_t frame_data = 0, frame_linesize = 64, frame_width = 104, frame_height = 108, frame_nb_samples = 112,
                frame_format = 116, frame_pts = 136, frame_sample_rate = 192, frame_ch_layout = 408; /* AVFrame */
    std::size_t pkt_pts = 8, pkt_dts = 16, pkt_data = 24, pkt_size = 32, pkt_stream_index = 36, pkt_flags = 40,
                pkt_duration = 64;                                             /* AVPacket */
    int codec_av1 = 225;                                                       /* AV_CODEC_ID_AV1 (226 in the 60 set) */
};
/* 60, 61 or 62 (anything else: the 61 offsets; the set is refused at load). */
Offsets offsets_for(int lavf_major);

/* The library sets tried, in order (one soname each; the ABI test checks them against the headers' majors). */
struct LibSet {
    const char *fmt, *codec, *util;
    int major;
};
constexpr int kLibSetCount = 3;
extern const LibSet kLibSets[kLibSetCount];

} // namespace av

struct AvLibs {
    std::string name; /* "libavformat.so.61 ..." */
    int lavf_major = 0;
    av::Offsets off;
    /* libavutil */
    void (*log_set_level)(int) = nullptr;
    av::Frame *(*frame_alloc)() = nullptr;
    void (*frame_free)(av::Frame **) = nullptr;
    int (*frame_get_buffer)(av::Frame *, int align) = nullptr;
    int (*frame_make_writable)(av::Frame *) = nullptr;
    int (*opt_set)(void *obj, const char *name, const char *val, int flags) = nullptr;
    int (*opt_set_int)(void *obj, const char *name, std::int64_t val, int flags) = nullptr;
    int (*opt_get_int)(void *obj, const char *name, int flags, std::int64_t *out) = nullptr;
    int (*opt_set_chlayout)(void *obj, const char *name, const av::ChannelLayout *layout, int flags) = nullptr;
    void (*chlayout_default)(av::ChannelLayout *, int nb_channels) = nullptr;
    int (*chlayout_copy)(av::ChannelLayout *dst, const av::ChannelLayout *src) = nullptr;
    void (*chlayout_uninit)(av::ChannelLayout *) = nullptr;
    int (*dict_set)(av::Dictionary **, const char *key, const char *value, int flags) = nullptr;
    void (*dict_free)(av::Dictionary **) = nullptr;
    int (*strerror)(int err, char *buf, std::size_t size) = nullptr;
    /* libavformat */
    int (*alloc_output_context2)(av::FormatContext **, const av::OutputFormat *, const char *format,
                                 const char *filename) = nullptr;
    void (*free_context)(av::FormatContext *) = nullptr;
    av::Stream *(*new_stream)(av::FormatContext *, const av::Codec *) = nullptr;
    int (*avio_open)(av::IOContext **, const char *url, int flags) = nullptr;
    int (*avio_closep)(av::IOContext **) = nullptr;
    void (*avio_flush)(av::IOContext *) = nullptr;
    int (*write_header)(av::FormatContext *, av::Dictionary **opts) = nullptr;
    int (*interleaved_write_frame)(av::FormatContext *, av::Packet *) = nullptr;
    int (*write_trailer)(av::FormatContext *) = nullptr;
    int (*open_input)(av::FormatContext **, const char *url, const av::InputFormat *, av::Dictionary **) = nullptr;
    int (*find_stream_info)(av::FormatContext *, av::Dictionary **) = nullptr;
    int (*read_frame)(av::FormatContext *, av::Packet *) = nullptr;
    void (*close_input)(av::FormatContext **) = nullptr;
    /* libavcodec */
    const av::Codec *(*find_encoder_by_name)(const char *) = nullptr;
    av::CodecContext *(*alloc_context)(const av::Codec *) = nullptr;
    void (*free_codec_context)(av::CodecContext **) = nullptr;
    int (*open2)(av::CodecContext *, const av::Codec *, av::Dictionary **) = nullptr;
    int (*send_frame)(av::CodecContext *, const av::Frame *) = nullptr;
    int (*receive_packet)(av::CodecContext *, av::Packet *) = nullptr;
    int (*parameters_from_context)(av::CodecParameters *, const av::CodecContext *) = nullptr;
    int (*parameters_copy)(av::CodecParameters *, const av::CodecParameters *) = nullptr;
    av::Packet *(*packet_alloc)() = nullptr;
    void (*packet_free)(av::Packet **) = nullptr;
    void (*packet_unref)(av::Packet *) = nullptr;
    /* playback (M5): the video decoder of a movie that is not MJPEG, the seek */
    const av::Codec *(*find_decoder)(int codec_id) = nullptr;
    int (*parameters_to_context)(av::CodecContext *, const av::CodecParameters *) = nullptr;
    int (*send_packet)(av::CodecContext *, const av::Packet *) = nullptr;
    int (*receive_frame)(av::CodecContext *, av::Frame *) = nullptr;
    void (*flush_buffers)(av::CodecContext *) = nullptr;
    int (*seek_frame)(av::FormatContext *, int stream_index, std::int64_t timestamp, int flags) = nullptr;
    bool complete() const;

    /* field access through the offsets */
    template <typename T> T &at(void *obj, std::size_t off) const
    {
        return *reinterpret_cast<T *>(static_cast<unsigned char *>(obj) + off);
    }
    av::IOContext **fmt_pb(av::FormatContext *f) const { return &at<av::IOContext *>(f, off.fmt_pb); }
    unsigned fmt_nb_streams(av::FormatContext *f) const { return at<unsigned>(f, off.fmt_nb_streams); }
    av::Stream *fmt_stream(av::FormatContext *f, unsigned i) const { return at<av::Stream **>(f, off.fmt_streams)[i]; }
    av::CodecParameters *stream_par(av::Stream *s) const { return at<av::CodecParameters *>(s, off.stream_codecpar); }
    av::Rational &stream_time_base(av::Stream *s) const { return at<av::Rational>(s, off.stream_time_base); }
    std::string error_text(int err) const;
};

/* Loaded on the first call, cached for the process (never unloaded). Null with `why` when missing. Thread-safe. */
const AvLibs *real_av_libs(std::string *why = nullptr);
bool av_libs_loaded();

constexpr const char *kAvPackages = "libavformat61 libavcodec61";

} // namespace vhs
