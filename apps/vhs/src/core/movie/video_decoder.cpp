/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a movie's video stream (see video_decoder.hpp).
 */
#include "video_decoder.hpp"

#include "live/convert.hpp"
#include "record/mkv_writer.hpp"
#include "scenes/index.hpp"

#include <algorithm>
#include <cstring>

namespace vhs {

VideoDecoder::~VideoDecoder()
{
    close();
}

void VideoDecoder::close()
{
    if (!av_) return;
    if (frame_) av_->frame_free(&frame_);
    if (pkt_) av_->packet_free(&pkt_);
    if (dec_) av_->free_codec_context(&dec_);
    if (ic_) av_->close_input(&ic_);
    vs_ = -1;
}

std::string VideoDecoder::codec_name() const
{
    return av_ ? codec_label(codec_, av_->lavf_major) : std::string("video");
}

bool VideoDecoder::open(const AvLibs &av, const std::string &path, int box_w, int box_h, PixFmt fmt, std::string *why)
{
    close();
    av_ = &av;
    box_w_ = box_w;
    box_h_ = box_h;
    fmt_ = fmt;
    if (av.open_input(&ic_, path.c_str(), nullptr, nullptr) < 0 || !ic_) {
        if (why) *why = "cannot open";
        ic_ = nullptr;
        return false;
    }
    if (av.find_stream_info(ic_, nullptr) < 0) {
        if (why) *why = "cannot read";
        close();
        return false;
    }
    const std::int64_t dur = av.at<std::int64_t>(ic_, av.off.fmt_duration);
    duration_ms_ = (dur != av::kNoPts && dur > 0) ? dur / 1000 : -1;
    const unsigned n = av.fmt_nb_streams(ic_);
    for (unsigned i = 0; i < n && vs_ < 0; ++i) {
        av::Stream *s = av.fmt_stream(ic_, i);
        av::CodecParameters *p = av.stream_par(s);
        if (av.at<int>(p, av.off.par_codec_type) != av::kMediaVideo) continue;
        vs_ = static_cast<int>(i);
        codec_ = av.at<int>(p, av.off.par_codec_id);
        w_ = av.at<int>(p, av.off.par_width);
        h_ = av.at<int>(p, av.off.par_height);
        tb_ = av.stream_time_base(s);
        const av::Rational fr = av.at<av::Rational>(s, av.off.stream_avg_frame_rate);
        if (fr.num > 0 && fr.den > 0) frame_ms_ = 1000.0 * fr.den / fr.num;
        mjpeg_ = codec_ == av::kCodecMjpeg;
        if (!mjpeg_) {
            const av::Codec *c = av.find_decoder(codec_);
            dec_ = c ? av.alloc_context(c) : nullptr;
            if (!dec_ || av.parameters_to_context(dec_, p) < 0) {
                if (why) *why = "This video cannot be played";
                close();
                return false;
            }
            av.opt_set_int(dec_, "threads", 2, 0); /* brief 4.4: frame threads 2-3 */
            if (av.open2(dec_, c, nullptr) < 0) {
                if (why) *why = "This video cannot be played";
                close();
                return false;
            }
        }
    }
    if (vs_ < 0) {
        if (why) *why = "no video stream";
        close();
        return false;
    }
    pkt_ = av.packet_alloc();
    frame_ = av.frame_alloc();
    eof_sent_ = false;
    return pkt_ && frame_;
}

int VideoDecoder::convert(av::Frame *f, Picture &out, std::string *why)
{
    const AvLibs &av = *av_;
    const int pf = av.at<int>(f, av.off.frame_format);
    const int fw = av.at<int>(f, av.off.frame_width), fh = av.at<int>(f, av.off.frame_height);
    YuvPlanes p;
    std::uint8_t **data = &av.at<std::uint8_t *>(f, av.off.frame_data);
    int *ls = &av.at<int>(f, av.off.frame_linesize);
    p.y = data[0];
    p.u = data[1];
    p.v = data[2];
    p.ys = ls[0];
    p.us = ls[1];
    p.vs = ls[2];
    if (pf == av::kPixYuv420p || pf == av::kPixYuvj420p) p.vshift = 1;
    else if (pf == av::kPixYuv422p || pf == av::kPixYuvj422p) p.vshift = 0;
    else if (pf == av::kPixNv12) p.nv12 = true;
    else {
        if (why) *why = "This video cannot be played";
        return Error;
    }
    p.full_range = pf == av::kPixYuvj420p || pf == av::kPixYuvj422p;
    int ow = 0, oh = 0;
    fit_size(fw, fh, box_w_, box_h_, ow, oh);
    out.fmt = fmt_;
    out.w = std::max(1, ow);
    out.h = std::max(1, oh);
    out.stride = out.w * pix_bytes(fmt_);
    out.box_w = box_w_;
    out.box_h = box_h_;
    out.dec_w = fw;
    out.dec_h = fh;
    out.px.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.h));
    yuv_fit(fmt_, p, fw, fh, out.px.data(), out.w, out.h, out.stride);
    return Picture_;
}

int VideoDecoder::next(Picture &out, std::int64_t &pts_ms, bool late, std::string *why)
{
    if (!ic_) return Error;
    const AvLibs &av = *av_;
    if (!mjpeg_ && late != skipping_nonref_) { /* the decoder skips non-reference frames while playback is late */
        av.opt_set(dec_, "skip_frame", late ? "nonref" : "default", 0);
        skipping_nonref_ = late;
    }
    for (int guard = 0; guard < 512; ++guard) {
        if (!mjpeg_) {
            const int r = av.receive_frame(dec_, frame_);
            if (r >= 0) {
                std::int64_t pts = av.at<std::int64_t>(frame_, av.off.frame_pts);
                pts_ms = pts == av::kNoPts ? 0 : rescale(pts, static_cast<std::int64_t>(tb_.num) * 1000, tb_.den);
                ++decoded_;
                if (skip_before_ms_ >= 0 && pts_ms + static_cast<std::int64_t>(frame_ms_ / 2) < skip_before_ms_) {
                    av.packet_unref(pkt_);
                    continue; /* decoded forward to the seek's target */
                }
                skip_before_ms_ = -1;
                const int c = convert(frame_, out, why);
                return c;
            }
            if (r == av::kErrEof) return End;
            if (r != av::kErrEagain) {
                if (why) *why = av.error_text(r);
                return Error;
            }
        }
        /* a packet */
        const int rr = av.read_frame(ic_, pkt_);
        if (rr < 0) {
            if (mjpeg_) return End;
            if (!eof_sent_) {
                eof_sent_ = true;
                av.send_packet(dec_, nullptr); /* drain */
            }
            continue;
        }
        if (av.at<int>(pkt_, av.off.pkt_stream_index) != vs_) {
            av.packet_unref(pkt_);
            continue;
        }
        if (mjpeg_) {
            const std::int64_t pts = av.at<std::int64_t>(pkt_, av.off.pkt_pts);
            pts_ms = pts == av::kNoPts ? 0 : rescale(pts, static_cast<std::int64_t>(tb_.num) * 1000, tb_.den);
            if (skip_before_ms_ >= 0 && pts_ms + static_cast<std::int64_t>(frame_ms_ / 2) < skip_before_ms_) {
                av.packet_unref(pkt_);
                continue; /* the exact frame after a seek */
            }
            skip_before_ms_ = -1;
            if (late) { /* the drop rule: an MJPEG frame is skipped before decoding */
                av.packet_unref(pkt_);
                ++dropped_;
                return Skipped;
            }
            const std::uint8_t *d = av.at<std::uint8_t *>(pkt_, av.off.pkt_data);
            const int n = av.at<int>(pkt_, av.off.pkt_size);
            std::string err;
            const bool ok = decode_jpeg_fit(d, static_cast<std::size_t>(n), box_w_, box_h_, fmt_, out, &err);
            av.packet_unref(pkt_);
            if (!ok) { /* a damaged frame: skip it */
                ++dropped_;
                return Skipped;
            }
            ++decoded_;
            out.box_w = box_w_;
            out.box_h = box_h_;
            return Picture_;
        }
        const int s = av.send_packet(dec_, pkt_);
        av.packet_unref(pkt_);
        if (s < 0 && s != av::kErrEagain) {
            if (why) *why = av.error_text(s);
            return Error;
        }
    }
    return Skipped;
}

bool VideoDecoder::seek(std::int64_t ms)
{
    if (!ic_) return false;
    const AvLibs &av = *av_;
    const std::int64_t ts = rescale(std::max<std::int64_t>(0, ms), tb_.den, static_cast<std::int64_t>(tb_.num) * 1000);
    if (av.seek_frame(ic_, vs_, ts, av::kSeekBackward) < 0) return false;
    if (dec_) av.flush_buffers(dec_);
    eof_sent_ = false;
    skip_before_ms_ = ms;
    return true;
}

bool first_picture(const AvLibs &av, const std::string &path, int box_w, int box_h, PixFmt fmt, Picture &out, std::string *why)
{
    VideoDecoder d;
    if (!d.open(av, path, box_w, box_h, fmt, why)) return false;
    std::int64_t pts = 0;
    for (int i = 0; i < 64; ++i) {
        const int r = d.next(out, pts, false, why);
        if (r == VideoDecoder::Picture_) return true;
        if (r < 0) return false;
    }
    if (why) *why = "no picture";
    return false;
}

} // namespace vhs
