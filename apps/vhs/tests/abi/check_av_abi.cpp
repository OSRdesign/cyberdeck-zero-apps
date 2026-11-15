/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the FFmpeg ABI check (brief 15.2). Built against one set of real FFmpeg headers (tests/abi/run.sh [root]):
 * every hand-declared offset, field size, constant and function signature of record/av_libs.hpp (mux, AAC encode and
 * the repair's demux) is compared with the headers, for the libavformat major they carry. Nothing is linked against
 * FFmpeg: the signatures are compared in unevaluated contexts, the offsets with offsetof.
 */
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include "record/av_libs.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

int g_ok = 0, g_bad = 0;

void same(const char *what, long long ours, long long theirs)
{
    if (ours == theirs) {
        ++g_ok;
        return;
    }
    ++g_bad;
    std::printf("MISMATCH %-34s ours %lld, the headers %lld\n", what, ours, theirs);
}

void same_s(const char *what, const std::string &ours, const std::string &theirs)
{
    if (ours == theirs) {
        ++g_ok;
        return;
    }
    ++g_bad;
    std::printf("MISMATCH %-34s ours %s, the headers %s\n", what, ours.c_str(), theirs.c_str());
}

/* a function's real signature, with the real types where av_libs.hpp has opaque ones */
#define SIG(fn, ...)                                                                                                   \
    static_assert(std::is_same<decltype(&fn), __VA_ARGS__>::value, #fn ": the signature differs from av_libs.hpp's");   \
    ++g_ok

#define OFF(field, T, member) same(#T "." #member, static_cast<long long>(o.field), static_cast<long long>(offsetof(T, member)))
#define SIZE(T, member, want) same("sizeof " #T "." #member, sizeof(static_cast<T *>(nullptr)->member), sizeof(want))

} // namespace

int main()
{
    using namespace vhs;
    const int major = LIBAVFORMAT_VERSION_MAJOR;
    std::printf("headers: libavformat %d.%d.%d, libavcodec %d.%d.%d, libavutil %d.%d.%d\n", LIBAVFORMAT_VERSION_MAJOR,
                LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO, LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR,
                LIBAVCODEC_VERSION_MICRO, LIBAVUTIL_VERSION_MAJOR, LIBAVUTIL_VERSION_MINOR, LIBAVUTIL_VERSION_MICRO);

    /* the set av_libs loads for this major: its sonames match the headers' majors */
    const av::LibSet *set = nullptr;
    for (const av::LibSet &s : av::kLibSets)
        if (s.major == major) set = &s;
    same("a library set for this major", set != nullptr, 1);
    if (set) {
        same_s("libavformat soname", set->fmt, "libavformat.so." + std::to_string(LIBAVFORMAT_VERSION_MAJOR));
        same_s("libavcodec soname", set->codec, "libavcodec.so." + std::to_string(LIBAVCODEC_VERSION_MAJOR));
        same_s("libavutil soname", set->util, "libavutil.so." + std::to_string(LIBAVUTIL_VERSION_MAJOR));
    }

    /* the offsets */
    const av::Offsets o = av::offsets_for(major);
    OFF(fmt_pb, AVFormatContext, pb);
    OFF(fmt_nb_streams, AVFormatContext, nb_streams);
    OFF(fmt_streams, AVFormatContext, streams);
    OFF(fmt_duration, AVFormatContext, duration);
    OFF(stream_codecpar, AVStream, codecpar);
    OFF(stream_time_base, AVStream, time_base);
    OFF(par_codec_type, AVCodecParameters, codec_type);
    OFF(par_codec_id, AVCodecParameters, codec_id);
    OFF(par_width, AVCodecParameters, width);
    OFF(par_height, AVCodecParameters, height);
    OFF(ctx_time_base, AVCodecContext, time_base);
    OFF(ctx_sample_fmt, AVCodecContext, sample_fmt);
    OFF(frame_data, AVFrame, data);
    OFF(frame_linesize, AVFrame, linesize);
    OFF(frame_nb_samples, AVFrame, nb_samples);
    OFF(frame_format, AVFrame, format);
    OFF(frame_pts, AVFrame, pts);
    OFF(frame_sample_rate, AVFrame, sample_rate);
    OFF(frame_ch_layout, AVFrame, ch_layout);
    OFF(pkt_pts, AVPacket, pts);
    OFF(pkt_dts, AVPacket, dts);
    OFF(pkt_data, AVPacket, data);
    OFF(pkt_size, AVPacket, size);
    OFF(pkt_stream_index, AVPacket, stream_index);
    OFF(pkt_flags, AVPacket, flags);
    OFF(pkt_duration, AVPacket, duration);

    /* the field sizes the accessors assume */
    SIZE(AVFormatContext, pb, void *);
    SIZE(AVFormatContext, nb_streams, unsigned);
    SIZE(AVFormatContext, streams, void *);
    SIZE(AVFormatContext, duration, std::int64_t);
    SIZE(AVStream, codecpar, void *);
    SIZE(AVStream, time_base, av::Rational);
    SIZE(AVCodecParameters, codec_type, int);
    SIZE(AVCodecParameters, codec_id, int);
    SIZE(AVCodecParameters, width, int);
    SIZE(AVCodecContext, time_base, av::Rational);
    SIZE(AVCodecContext, sample_fmt, int);
    SIZE(AVFrame, data, std::uint8_t * [8]);
    SIZE(AVFrame, nb_samples, int);
    SIZE(AVFrame, format, int);
    SIZE(AVFrame, pts, std::int64_t);
    SIZE(AVFrame, sample_rate, int);
    SIZE(AVFrame, ch_layout, av::ChannelLayout);
    SIZE(AVPacket, pts, std::int64_t);
    SIZE(AVPacket, dts, std::int64_t);
    SIZE(AVPacket, data, void *);
    SIZE(AVPacket, size, int);
    SIZE(AVPacket, stream_index, int);
    SIZE(AVPacket, flags, int);
    SIZE(AVPacket, duration, std::int64_t);
    same("AV_NUM_DATA_POINTERS", 8, AV_NUM_DATA_POINTERS);
    same("sizeof AVRational", sizeof(av::Rational), sizeof(AVRational));
    same("offsetof AVRational.den", offsetof(av::Rational, den), offsetof(AVRational, den));
    same("sizeof AVChannelLayout", sizeof(av::ChannelLayout), sizeof(AVChannelLayout));
    same("offsetof AVChannelLayout.order", offsetof(av::ChannelLayout, order), offsetof(AVChannelLayout, order));
    same("offsetof AVChannelLayout.nb_channels", offsetof(av::ChannelLayout, nb_channels), offsetof(AVChannelLayout, nb_channels));
    same("offsetof AVChannelLayout.u.mask", offsetof(av::ChannelLayout, mask), offsetof(AVChannelLayout, u.mask));
    same("offsetof AVChannelLayout.opaque", offsetof(av::ChannelLayout, opaque), offsetof(AVChannelLayout, opaque));

    /* the constants */
    same("AVMEDIA_TYPE_VIDEO", av::kMediaVideo, AVMEDIA_TYPE_VIDEO);
    same("AVMEDIA_TYPE_AUDIO", av::kMediaAudio, AVMEDIA_TYPE_AUDIO);
    same("AV_CODEC_ID_MJPEG", av::kCodecMjpeg, AV_CODEC_ID_MJPEG);
    same("AV_CODEC_ID_H264", av::kCodecH264, AV_CODEC_ID_H264);
    same("AV_CODEC_ID_AAC", av::kCodecAac, AV_CODEC_ID_AAC);
    same("AV_CODEC_ID_HEVC", av::kCodecHevc, AV_CODEC_ID_HEVC);
    same("AV_CODEC_ID_MPEG4", av::kCodecMpeg4, AV_CODEC_ID_MPEG4);
    same("AV_CODEC_ID_VP8", av::kCodecVp8, AV_CODEC_ID_VP8);
    same("AV_CODEC_ID_VP9", av::kCodecVp9, AV_CODEC_ID_VP9);
    same("AV_CODEC_ID_AV1 (per set)", o.codec_av1, AV_CODEC_ID_AV1);
    same("AV_TIME_BASE (us)", 1000000, AV_TIME_BASE);
    same("AVIO_FLAG_WRITE", av::kIoWrite, AVIO_FLAG_WRITE);
    same("AV_PKT_FLAG_KEY", av::kPktKey, AV_PKT_FLAG_KEY);
    same("AV_SAMPLE_FMT_FLTP", av::kFmtFltp, AV_SAMPLE_FMT_FLTP);
    same("AVERROR(EAGAIN)", av::kErrEagain, AVERROR(EAGAIN));
    same("AVERROR_EOF", av::kErrEof, AVERROR_EOF);
    same("AV_LOG_QUIET", av::kLogQuiet, AV_LOG_QUIET);
    same("AV_LOG_ERROR", av::kLogError, AV_LOG_ERROR);
    same("AV_NOPTS_VALUE", av::kNoPts, AV_NOPTS_VALUE);

    /* the functions av_libs resolves, with the parameter lists it calls them with */
    SIG(av_log_set_level, void (*)(int));
    SIG(av_frame_alloc, AVFrame *(*)());
    SIG(av_frame_free, void (*)(AVFrame **));
    SIG(av_frame_get_buffer, int (*)(AVFrame *, int));
    SIG(av_frame_make_writable, int (*)(AVFrame *));
    SIG(av_opt_set, int (*)(void *, const char *, const char *, int));
    SIG(av_opt_set_int, int (*)(void *, const char *, std::int64_t, int));
    SIG(av_opt_get_int, int (*)(void *, const char *, int, std::int64_t *));
    SIG(av_opt_set_chlayout, int (*)(void *, const char *, const AVChannelLayout *, int));
    SIG(av_channel_layout_default, void (*)(AVChannelLayout *, int));
    SIG(av_channel_layout_copy, int (*)(AVChannelLayout *, const AVChannelLayout *));
    SIG(av_channel_layout_uninit, void (*)(AVChannelLayout *));
    SIG(av_dict_set, int (*)(AVDictionary **, const char *, const char *, int));
    SIG(av_dict_free, void (*)(AVDictionary **));
    SIG(av_strerror, int (*)(int, char *, std::size_t));
    SIG(avformat_alloc_output_context2, int (*)(AVFormatContext **, const AVOutputFormat *, const char *, const char *));
    SIG(avformat_free_context, void (*)(AVFormatContext *));
    SIG(avformat_new_stream, AVStream *(*)(AVFormatContext *, const AVCodec *));
    SIG(avio_open, int (*)(AVIOContext **, const char *, int));
    SIG(avio_closep, int (*)(AVIOContext **));
    SIG(avio_flush, void (*)(AVIOContext *));
    SIG(avformat_write_header, int (*)(AVFormatContext *, AVDictionary **));
    SIG(av_interleaved_write_frame, int (*)(AVFormatContext *, AVPacket *));
    SIG(av_write_trailer, int (*)(AVFormatContext *));
    SIG(avformat_open_input, int (*)(AVFormatContext **, const char *, const AVInputFormat *, AVDictionary **));
    SIG(avformat_find_stream_info, int (*)(AVFormatContext *, AVDictionary **));
    SIG(av_read_frame, int (*)(AVFormatContext *, AVPacket *));
    SIG(avformat_close_input, void (*)(AVFormatContext **));
    SIG(avcodec_find_encoder_by_name, const AVCodec *(*)(const char *));
    SIG(avcodec_alloc_context3, AVCodecContext *(*)(const AVCodec *));
    SIG(avcodec_free_context, void (*)(AVCodecContext **));
    SIG(avcodec_open2, int (*)(AVCodecContext *, const AVCodec *, AVDictionary **));
    SIG(avcodec_send_frame, int (*)(AVCodecContext *, const AVFrame *));
    SIG(avcodec_receive_packet, int (*)(AVCodecContext *, AVPacket *));
    SIG(avcodec_parameters_from_context, int (*)(AVCodecParameters *, const AVCodecContext *));
    SIG(avcodec_parameters_copy, int (*)(AVCodecParameters *, const AVCodecParameters *));
    SIG(av_packet_alloc, AVPacket *(*)());
    SIG(av_packet_free, void (*)(AVPacket **));
    SIG(av_packet_unref, void (*)(AVPacket *));
    /* the AVOptions set on the AAC context by name exist in these headers' option table only at run time: the unit
     * tests' recorder fixtures open the encoder for real (FFmpeg 6.1 there); "sample_fmt" has no AVOption and goes
     * through ctx_sample_fmt above */

    std::printf("abi %d: %d checks, %d mismatches: %s\n", major, g_ok + g_bad, g_bad, g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}
