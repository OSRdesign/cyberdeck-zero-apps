/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl: the hand-written ABI of decode_libs.hpp against the real headers (libmpg123, libvorbisfile, FFmpeg), compiled
 * by tests/abi/run.sh with the -dev packages of one FFmpeg version (a private root: apt-get download, dpkg-deb -x).
 * Prints each check; exit status 1 on a difference. Not part of test-vinyl (it needs the headers).
 */
#include "decode/decode_libs.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <mpg123.h>
#include <vorbis/vorbisfile.h>
}

#include <cstddef>
#include <cstdio>

static int fails = 0;
#define EQ(a, b)                                                                                                       \
    do {                                                                                                               \
        const long long x_ = static_cast<long long>(a), y_ = static_cast<long long>(b);                                 \
        std::printf("%-58s %lld %s %lld\n", #a, x_, x_ == y_ ? "==" : "!=", y_);                                      \
        if (x_ != y_) ++fails;                                                                                         \
    } while (0)

int main() {
    using namespace vinyl;
    /* mpg123 */
    EQ(mpg::kOk, MPG123_OK);
    EQ(mpg::kDone, MPG123_DONE);
    EQ(mpg::kNewFormat, MPG123_NEW_FORMAT);
    EQ(mpg::kNeedMore, MPG123_NEED_MORE);
    EQ(mpg::kErr, MPG123_ERR);
    EQ(mpg::kParamFlags, MPG123_FLAGS);
    EQ(mpg::kParamAddFlags, MPG123_ADD_FLAGS);
    EQ(mpg::kParamRemoveFlags, MPG123_REMOVE_FLAGS);
    EQ(mpg::kFlagQuiet, MPG123_QUIET);
    EQ(mpg::kFlagGapless, MPG123_GAPLESS);
    EQ(mpg::kFlagForceStereo, MPG123_FORCE_STEREO);
    EQ(mpg::kMono, MPG123_MONO);
    EQ(mpg::kStereo, MPG123_STEREO);
    EQ(mpg::kEncSigned16, MPG123_ENC_SIGNED_16);
    EQ(sizeof(off_t), 8);
    /* vorbisfile */
    EQ(ov::kFileBytes >= sizeof(OggVorbis_File), 1);
    EQ(offsetof(vorbis_info, channels), offsetof(ov::Info, channels));
    EQ(offsetof(vorbis_info, rate), offsetof(ov::Info, rate));
    EQ(sizeof(ov_callbacks), sizeof(ov::Callbacks));
    /* FFmpeg */
    const av::Offsets o = av::offsets_for(LIBAVFORMAT_VERSION_MAJOR);
    std::printf("libavformat %d, libavcodec %d, libavutil %d, libswresample %d\n", LIBAVFORMAT_VERSION_MAJOR,
                LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR);
    EQ(o.fmt_nb_streams, offsetof(AVFormatContext, nb_streams));
    EQ(o.fmt_streams, offsetof(AVFormatContext, streams));
    EQ(o.stream_codecpar, offsetof(AVStream, codecpar));
    EQ(o.stream_time_base, offsetof(AVStream, time_base));
    EQ(o.stream_duration, offsetof(AVStream, duration));
    EQ(o.packet_stream_index, offsetof(AVPacket, stream_index));
    EQ(o.frame_extended_data, offsetof(AVFrame, extended_data));
    EQ(o.frame_nb_samples, offsetof(AVFrame, nb_samples));
    EQ(o.frame_format, offsetof(AVFrame, format));
    EQ(offsetof(AVCodec, name), 0);
    EQ(sizeof(AVChannelLayout), sizeof(av::ChannelLayout));
    EQ(offsetof(AVChannelLayout, nb_channels), offsetof(av::ChannelLayout, nb_channels));
    EQ(av::kMediaAudio, AVMEDIA_TYPE_AUDIO);
    EQ(av::kErrEagain, AVERROR(EAGAIN));
    EQ(av::kErrEof, AVERROR_EOF);
    EQ(av::kSeekBackward, AVSEEK_FLAG_BACKWARD);
    EQ(av::kSearchChildren, AV_OPT_SEARCH_CHILDREN);
    EQ(av::kLogQuiet, AV_LOG_QUIET);
    EQ(av::kNoPts, AV_NOPTS_VALUE);
    EQ(av::kS16, AV_SAMPLE_FMT_S16);
    EQ(av::kS32, AV_SAMPLE_FMT_S32);
    EQ(av::kFltP, AV_SAMPLE_FMT_FLTP);
    EQ(av::kS16P, AV_SAMPLE_FMT_S16P);
    EQ(av::kS32P, AV_SAMPLE_FMT_S32P);
    EQ(av::kU8, AV_SAMPLE_FMT_U8);
    EQ(av::kU8P, AV_SAMPLE_FMT_U8P);
    std::printf(fails ? "ABI: %d difference(s)\n" : "ABI: all equal\n", fails);
    return fails ? 1 : 0;
}
