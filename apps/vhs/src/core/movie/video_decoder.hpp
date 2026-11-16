/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a movie's video stream, demuxed and decoded to fit the picture box (brief 4.4, 10). Its own libavformat demuxer
 * (the audio is Vinyl's engine's, on a second demuxer of the same file: brief section 20). MJPEG (VHS's scenes): each
 * packet is a JPEG, decoded by TurboJPEG at the box scale (the live view's fit rule). Other codecs: FFmpeg's software
 * decoder (2 frame threads), YUV420P / YUVJ420P / YUV422P / YUVJ422P / NV12 converted by the own YUV-to-RGB with fit
 * (live/convert.hpp; no libswscale); any other pixel format, or no decoder: "cannot be played". Late frames: an MJPEG
 * packet is skipped before decoding; another codec's decoder is told to skip non-reference frames until it catches up
 * (AVOption skip_frame). Seeking: MJPEG to the exact frame (the cues bring the reader back to the cluster, then it reads
 * forward); others to the keyframe before, then decoded forward to the target. One owner: the movie thread (or the
 * scenes worker for a thumbnail). LVGL-free.
 */
#pragma once

#include "live/pipeline.hpp"
#include "record/av_libs.hpp"

#include <cstdint>
#include <string>

namespace vhs {

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    /* false with why ("no video stream", "This video cannot be played": no decoder) */
    bool open(const AvLibs &av, const std::string &path, int box_w, int box_h, PixFmt fmt, std::string *why);
    void close();
    enum Result { Picture_ = 1, Skipped = 0, End = -1, Error = -2 };
    /* The next frame: Picture_ with out filled and pts_ms; Skipped when `late` dropped it (MJPEG) or the decoder needs
     * more input; End at the end; Error (why). For FFmpeg codecs `late` switches non-reference skipping on / off. */
    int next(Picture &out, std::int64_t &pts_ms, bool late, std::string *why = nullptr);
    /* Positions the reader so the next picture is the frame at (MJPEG) or after the keyframe before (others) ms;
     * frames before `ms` are then skipped by next() until it is reached (the exact target). */
    bool seek(std::int64_t ms);

    bool mjpeg() const { return mjpeg_; }
    int codec_id() const { return codec_; }
    int width() const { return w_; }
    int height() const { return h_; }
    std::int64_t duration_ms() const { return duration_ms_; }
    double frame_ms() const { return frame_ms_; }    /* one frame period (33.3 at 30 fps) */
    std::string codec_name() const;                  /* "MJPEG", "H.264" ... */
    std::uint64_t decoded() const { return decoded_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    int convert(av::Frame *f, Picture &out, std::string *why);
    const AvLibs *av_ = nullptr;
    av::FormatContext *ic_ = nullptr;
    av::CodecContext *dec_ = nullptr;
    av::Packet *pkt_ = nullptr;
    av::Frame *frame_ = nullptr;
    int vs_ = -1, codec_ = -1, w_ = 0, h_ = 0;
    av::Rational tb_{1, 1000};
    bool mjpeg_ = false;
    int box_w_ = 0, box_h_ = 0;
    PixFmt fmt_ = PixFmt::RGB565;
    std::int64_t duration_ms_ = -1;
    double frame_ms_ = 1000.0 / 30;
    std::int64_t skip_before_ms_ = -1; /* after a seek: frames before it are not shown */
    bool skipping_nonref_ = false;
    bool eof_sent_ = false;
    std::uint64_t decoded_ = 0, dropped_ = 0;
};

/* The first picture of a movie of any codec fitted to a box (the scenes list's thumbnails for codecs other than MJPEG). */
bool first_picture(const AvLibs &av, const std::string &path, int box_w, int box_h, PixFmt fmt, Picture &out, std::string *why);

} // namespace vhs
