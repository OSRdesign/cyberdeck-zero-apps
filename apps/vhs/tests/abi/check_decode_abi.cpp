/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829, tests/abi/check_decode_abi.cpp) for VHS (brief 3.1, 10, 15.2); VHS changes are
 * marked VHS.
 *
 * VHS: the hand-written FFmpeg ABI of play/decode/decode_libs.hpp (the audio decoder of playback: demux, decode, resample)
 * against the real headers, for the libavformat major they carry (tests/abi/run.sh, every root: the 60, 61 and 62 sets). Vinyl's
 * mpg123 and vorbisfile checks are gone with those libraries. Prints each check; exit status 1 on a difference. Nothing is linked
 * against FFmpeg: the signatures are compared in unevaluated contexts, the offsets with offsetof.
 *
 * VHS additions to Vinyl's check: AVFormatContext's start_time and duration, AVPacket's pts, AVERROR_STREAM_NOT_FOUND, the library
 * sets' sonames, and every function pointer of FfmpegLib against the real function's signature (the real parameter types are
 * mapped onto the opaque ones decode_libs.hpp declares: enums become int, AVInputFormat and AVCodecParameters become void).
 */
#include "play/decode/decode_libs.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

using namespace vhs;

static int fails = 0, oks = 0;
#define EQ(a, b)                                                                                                       \
    do {                                                                                                               \
        const long long x_ = static_cast<long long>(a), y_ = static_cast<long long>(b);                                 \
        std::printf("%-58s %lld %s %lld\n", #a, x_, x_ == y_ ? "==" : "!=", y_);                                      \
        if (x_ != y_) ++fails;                                                                                         \
        else ++oks;                                                                                                    \
    } while (0)

/* the real types as decode_libs.hpp spells them */
template <typename T> struct C { using type = T; };
template <> struct C<AVFormatContext> { using type = dav::FormatContext; };
template <> struct C<AVCodecContext> { using type = dav::CodecContext; };
template <> struct C<AVCodec> { using type = dav::Codec; };
template <> struct C<AVPacket> { using type = dav::Packet; };
template <> struct C<AVFrame> { using type = dav::Frame; };
template <> struct C<SwrContext> { using type = dav::Swr; };
template <> struct C<AVDictionary> { using type = dav::Dictionary; };
template <> struct C<AVChannelLayout> { using type = dav::ChannelLayout; };
template <> struct C<AVInputFormat> { using type = void; };
template <> struct C<AVCodecParameters> { using type = void; };
template <> struct C<AVSampleFormat> { using type = int; };
template <> struct C<AVMediaType> { using type = int; };
template <typename T> struct C<T *> { using type = typename C<T>::type *; };
template <typename T> struct C<const T> { using type = const typename C<T>::type; };
/* a const pointer is the same ABI as a pointer (FFmpeg 7 spells swr_convert's planes `uint8_t *const *`, 6.1 `uint8_t **`) */
template <typename T> struct C<T *const> { using type = typename C<T>::type *; };
template <typename F> struct CF;
template <typename R, typename... A> struct CF<R (*)(A...)> { using type = typename C<R>::type (*)(typename C<A>::type...); };

#define SIG(real, field)                                                                                               \
    do {                                                                                                               \
        static_assert(std::is_same<CF<decltype(&real)>::type, decltype(FfmpegLib::field)>::value,                      \
                      #real ": the signature differs from decode_libs.hpp's FfmpegLib::" #field);                      \
        ++oks;                                                                                                         \
    } while (0)

int main() {
    const dav::Offsets o = dav::offsets_for(LIBAVFORMAT_VERSION_MAJOR);
    std::printf("libavformat %d, libavcodec %d, libavutil %d, libswresample %d\n", LIBAVFORMAT_VERSION_MAJOR,
                LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR);

    /* the set decode_libs loads for this major: its sonames match the headers' majors */
    const dav::LibSet *set = nullptr;
    for (const dav::LibSet &s : dav::kLibSets)
        if (s.major == LIBAVFORMAT_VERSION_MAJOR) set = &s;
    EQ(set != nullptr, 1);
    if (set) {
        EQ(std::string(set->fmt) == "libavformat.so." + std::to_string(LIBAVFORMAT_VERSION_MAJOR), 1);
        EQ(std::string(set->codec) == "libavcodec.so." + std::to_string(LIBAVCODEC_VERSION_MAJOR), 1);
        EQ(std::string(set->util) == "libavutil.so." + std::to_string(LIBAVUTIL_VERSION_MAJOR), 1);
        EQ(std::string(set->swr) == "libswresample.so." + std::to_string(LIBSWRESAMPLE_VERSION_MAJOR), 1);
    }

    /* the offsets */
    EQ(o.fmt_nb_streams, offsetof(AVFormatContext, nb_streams));
    EQ(o.fmt_streams, offsetof(AVFormatContext, streams));
    EQ(o.fmt_start_time, offsetof(AVFormatContext, start_time));
    EQ(o.fmt_duration, offsetof(AVFormatContext, duration));
    EQ(sizeof(static_cast<AVFormatContext *>(nullptr)->start_time), sizeof(int64_t));
    EQ(sizeof(static_cast<AVFormatContext *>(nullptr)->duration), sizeof(int64_t));
    EQ(o.stream_codecpar, offsetof(AVStream, codecpar));
    EQ(o.stream_time_base, offsetof(AVStream, time_base));
    EQ(o.stream_duration, offsetof(AVStream, duration));
    EQ(o.packet_pts, offsetof(AVPacket, pts));
    EQ(o.packet_stream_index, offsetof(AVPacket, stream_index));
    EQ(o.frame_extended_data, offsetof(AVFrame, extended_data));
    EQ(o.frame_nb_samples, offsetof(AVFrame, nb_samples));
    EQ(o.frame_format, offsetof(AVFrame, format));
    EQ(offsetof(AVCodec, name), 0);
    EQ(sizeof(AVChannelLayout), sizeof(dav::ChannelLayout));
    EQ(offsetof(AVChannelLayout, nb_channels), offsetof(dav::ChannelLayout, nb_channels));
    EQ(sizeof(AVRational), sizeof(dav::Rational));
    EQ(offsetof(AVRational, den), offsetof(dav::Rational, den));

    /* the constants */
    EQ(dav::kMediaAudio, AVMEDIA_TYPE_AUDIO);
    EQ(dav::kErrEagain, AVERROR(EAGAIN));
    EQ(dav::kErrEof, AVERROR_EOF);
    EQ(dav::kErrStreamNotFound, AVERROR_STREAM_NOT_FOUND);
    EQ(dav::kSeekBackward, AVSEEK_FLAG_BACKWARD);
    EQ(dav::kSearchChildren, AV_OPT_SEARCH_CHILDREN);
    EQ(dav::kLogQuiet, AV_LOG_QUIET);
    EQ(dav::kNoPts, AV_NOPTS_VALUE);
    EQ(dav::kU8, AV_SAMPLE_FMT_U8);
    EQ(dav::kS16, AV_SAMPLE_FMT_S16);
    EQ(dav::kS32, AV_SAMPLE_FMT_S32);
    EQ(dav::kFlt, AV_SAMPLE_FMT_FLT);
    EQ(dav::kDbl, AV_SAMPLE_FMT_DBL);
    EQ(dav::kU8P, AV_SAMPLE_FMT_U8P);
    EQ(dav::kS16P, AV_SAMPLE_FMT_S16P);
    EQ(dav::kS32P, AV_SAMPLE_FMT_S32P);
    EQ(dav::kFltP, AV_SAMPLE_FMT_FLTP);
    EQ(dav::kDblP, AV_SAMPLE_FMT_DBLP);
    EQ(AV_TIME_BASE, 1000000);

    /* the functions decode_libs resolves, with the parameter lists it calls them with */
    SIG(avutil_version, avutil_version);
    SIG(av_log_set_level, log_set_level);
    SIG(av_frame_alloc, frame_alloc);
    SIG(av_frame_free, frame_free);
    SIG(av_frame_unref, frame_unref);
    SIG(av_opt_get_int, opt_get_int);
    SIG(av_opt_get_sample_fmt, opt_get_sample_fmt);
    SIG(av_opt_get_chlayout, opt_get_chlayout);
    SIG(av_channel_layout_default, chlayout_default);
    SIG(av_channel_layout_uninit, chlayout_uninit);
    SIG(avformat_version, avformat_version);
    SIG(avformat_open_input, open_input);
    SIG(avformat_find_stream_info, find_stream_info);
    SIG(av_find_best_stream, find_best_stream);
    SIG(avformat_close_input, close_input);
    SIG(av_read_frame, read_frame);
    SIG(av_seek_frame, seek_frame);
    SIG(avcodec_alloc_context3, alloc_context);
    SIG(avcodec_free_context, free_context);
    SIG(avcodec_parameters_to_context, parameters_to_context);
    SIG(avcodec_open2, open2);
    SIG(avcodec_send_packet, send_packet);
    SIG(avcodec_receive_frame, receive_frame);
    SIG(avcodec_flush_buffers, flush_buffers);
    SIG(av_packet_alloc, packet_alloc);
    SIG(av_packet_free, packet_free);
    SIG(av_packet_unref, packet_unref);
    SIG(swr_alloc_set_opts2, swr_alloc_set_opts2);
    SIG(swr_init, swr_init);
    SIG(swr_free, swr_free);
    SIG(swr_convert, swr_convert);

    std::printf(fails ? "decode ABI: %d difference(s)\n" : "decode ABI: all %d checks equal\n", fails ? fails : oks);
    return fails ? 1 : 0;
}
