/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the MKV file and the AAC encoder (see mkv_writer.hpp).
 */
#include "mkv_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vhs {

const char *video_codec_name(VideoCodec c)
{
    return c == VideoCodec::H264 ? "H.264" : "MJPEG";
}

std::int64_t rescale(std::int64_t x, std::int64_t num, std::int64_t den)
{
    if (den == 0) return 0;
    const __int128 v = static_cast<__int128>(x) * num;
    const __int128 half = (den > 0 ? den : -den) / 2;
    return static_cast<std::int64_t>(v >= 0 ? (v + half) / den : (v - half) / den);
}

void s16_to_float_planar(const std::int16_t *in, std::size_t frames, int channels, float *const *planes)
{
    constexpr float k = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) planes[c][i] = static_cast<float>(in[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]) * k;
}

/* --------------------------------------------------------------------------------------------------- AacEncoder */

AacEncoder::~AacEncoder()
{
    if (!av_) return;
    if (pkt_) av_->packet_free(&pkt_);
    if (frame_) av_->frame_free(&frame_);
    if (ctx_) av_->free_codec_context(&ctx_);
}

bool AacEncoder::open(const AvLibs &av, int rate, int channels, std::string *err)
{
    av_ = &av;
    rate_ = rate;
    channels_ = channels;
    const av::Codec *codec = av.find_encoder_by_name("aac");
    if (!codec) {
        if (err) *err = "FFmpeg has no aac encoder";
        return false;
    }
    ctx_ = av.alloc_context(codec);
    if (!ctx_) {
        if (err) *err = "no codec context";
        return false;
    }
    av::ChannelLayout layout{};
    av.chlayout_default(&layout, channels);
    const char *step = "ar";
    int r = av.opt_set_int(ctx_, "ar", rate, 0);
    av.at<int>(ctx_, av.off.ctx_sample_fmt) = av::kFmtFltp; /* the native aac encoder takes float planar only */
    if (r >= 0) r = av.opt_set_chlayout(ctx_, step = "ch_layout", &layout, 0);
    if (r >= 0) r = av.opt_set_int(ctx_, step = "b", bit_rate_for(rate), 0);
    if (r >= 0) r = av.opt_set(ctx_, step = "flags", "+global_header", 0); /* MKV keeps the AudioSpecificConfig in the header */
    av.at<av::Rational>(ctx_, av.off.ctx_time_base) = av::Rational{1, rate};
    if (r >= 0) {
        step = "open";
        r = av.open2(ctx_, codec, nullptr);
    }
    av.chlayout_uninit(&layout);
    if (r < 0) {
        if (err) *err = std::string("aac: ") + step + ": " + av.error_text(r);
        return false;
    }
    std::int64_t fs = 0;
    if (av.opt_get_int(ctx_, "frame_size", 0, &fs) >= 0 && fs > 0) frame_size_ = static_cast<int>(fs);
    frame_ = av.frame_alloc();
    pkt_ = av.packet_alloc();
    if (!frame_ || !pkt_) {
        if (err) *err = "no frame or packet";
        return false;
    }
    av.at<int>(frame_, av.off.frame_nb_samples) = frame_size_;
    av.at<int>(frame_, av.off.frame_format) = av::kFmtFltp;
    av.at<int>(frame_, av.off.frame_sample_rate) = rate;
    av::ChannelLayout fl{};
    av.chlayout_default(&fl, channels);
    av.chlayout_copy(&av.at<av::ChannelLayout>(frame_, av.off.frame_ch_layout), &fl);
    av.chlayout_uninit(&fl);
    r = av.frame_get_buffer(frame_, 0);
    if (r < 0) {
        if (err) *err = "aac frame: " + av.error_text(r);
        return false;
    }
    return true;
}

int AacEncoder::drain_packets()
{
    for (;;) {
        const int r = av_->receive_packet(ctx_, pkt_);
        if (r == av::kErrEagain || r == av::kErrEof) return 0;
        if (r < 0) return r;
        EncodedAudio e;
        const std::uint8_t *d = av_->at<std::uint8_t *>(pkt_, av_->off.pkt_data);
        const int n = av_->at<int>(pkt_, av_->off.pkt_size);
        e.data.assign(d, d + n);
        e.pts = av_->at<std::int64_t>(pkt_, av_->off.pkt_pts);
        e.duration = av_->at<std::int64_t>(pkt_, av_->off.pkt_duration);
        if (e.duration <= 0) e.duration = frame_size_;
        out_.push_back(std::move(e));
        av_->packet_unref(pkt_);
    }
}

int AacEncoder::send_current(int nb)
{
    av_->at<int>(frame_, av_->off.frame_nb_samples) = nb;
    av_->at<std::int64_t>(frame_, av_->off.frame_pts) = next_pts_;
    int r = av_->send_frame(ctx_, frame_);
    if (r < 0) return r;
    next_pts_ += nb;
    fill_ = 0;
    r = drain_packets();
    if (r < 0) return r;
    av_->at<int>(frame_, av_->off.frame_nb_samples) = frame_size_;
    return av_->frame_make_writable(frame_);
}

int AacEncoder::push(const std::int16_t *in, std::size_t frames)
{
    if (!ctx_ || finished_) return -1;
    samples_in_ += static_cast<std::int64_t>(frames);
    std::size_t done = 0;
    float *planes[8];
    while (done < frames) {
        const std::size_t room = static_cast<std::size_t>(frame_size_ - fill_);
        const std::size_t n = std::min(room, frames - done);
        for (int c = 0; c < channels_ && c < 8; ++c)
            planes[c] = reinterpret_cast<float *>(av_->at<std::uint8_t *>(frame_, av_->off.frame_data + 8 * static_cast<std::size_t>(c))) + fill_;
        s16_to_float_planar(in + done * static_cast<std::size_t>(channels_), n, channels_, planes);
        fill_ += static_cast<int>(n);
        done += n;
        if (fill_ == frame_size_) {
            const int r = send_current(frame_size_);
            if (r < 0) return r;
        }
    }
    return 0;
}

int AacEncoder::finish()
{
    if (!ctx_ || finished_) return 0;
    finished_ = true;
    if (fill_ > 0) {
        const int r = send_current(fill_);
        if (r < 0) return r;
    }
    int r = av_->send_frame(ctx_, nullptr);
    if (r < 0 && r != av::kErrEof) return r;
    return drain_packets();
}

bool AacEncoder::pop(EncodedAudio &out)
{
    if (out_.empty()) return false;
    out = std::move(out_.front());
    out_.pop_front();
    return true;
}

/* ----------------------------------------------------------------------------------------------------- MkvMuxer */

MkvMuxer::~MkvMuxer()
{
    if (fmt_) close(nullptr);
}

bool MkvMuxer::open(const AvLibs &av, const std::string &path, const VideoFormat &v, const AacEncoder *audio, std::string *err)
{
    av_ = &av;
    int r = av.alloc_output_context2(&fmt_, nullptr, "matroska", path.c_str());
    if (r < 0 || !fmt_) {
        if (err) *err = "matroska: " + av.error_text(r);
        fmt_ = nullptr;
        return false;
    }
    auto fail = [&](const std::string &why) {
        if (err) *err = why;
        av.free_context(fmt_);
        fmt_ = nullptr;
        return false;
    };
    /* the writer orders the packets itself; when the microphone goes away mid-scene the muxer holds video for 1 s at
     * most waiting for audio (the default is 10 s: 300 frames in memory) */
    av.opt_set_int(fmt_, "max_interleave_delta", 1000000, 0);
    av::Stream *vs = av.new_stream(fmt_, nullptr);
    if (!vs) return fail("no video stream");
    av::CodecParameters *vp = av.stream_par(vs);
    av.at<int>(vp, av.off.par_codec_type) = av::kMediaVideo;
    av.at<int>(vp, av.off.par_codec_id) = v.codec == VideoCodec::H264 ? av::kCodecH264 : av::kCodecMjpeg;
    av.at<int>(vp, av.off.par_width) = v.w;
    av.at<int>(vp, av.off.par_height) = v.h;
    av.stream_time_base(vs) = av::Rational{1, 1000};
    audio_index_ = -1;
    if (audio && audio->context()) {
        av::Stream *as = av.new_stream(fmt_, nullptr);
        if (!as) return fail("no audio stream");
        r = av.parameters_from_context(av.stream_par(as), audio->context());
        if (r < 0) return fail("audio parameters: " + av.error_text(r));
        av.stream_time_base(as) = av::Rational{1, 1000};
        audio_index_ = 1;
    }
    r = av.avio_open(av.fmt_pb(fmt_), path.c_str(), av::kIoWrite);
    if (r < 0) return fail(path + ": " + av.error_text(r));
    av::Dictionary *opts = nullptr;
    av.dict_set(&opts, "cluster_time_limit", "1000", 0); /* a crash loses about 1 s at most */
    r = av.write_header(fmt_, &opts);
    av.dict_free(&opts);
    if (r < 0) {
        av.avio_closep(av.fmt_pb(fmt_));
        return fail("header: " + av.error_text(r));
    }
    header_ = true;
    for (unsigned i = 0; i < av.fmt_nb_streams(fmt_) && i < 2; ++i) tb_[i] = av.stream_time_base(av.fmt_stream(fmt_, i));
    pkt_ = av.packet_alloc();
    if (!pkt_) return fail("no packet");
    return true;
}

int MkvMuxer::write(int stream, const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms, bool key)
{
    if (!fmt_ || !pkt_) return -1;
    const av::Rational tb = tb_[stream];
    const AvLibs &av = *av_;
    av.at<std::uint8_t *>(pkt_, av.off.pkt_data) = const_cast<std::uint8_t *>(data); /* not refcounted: FFmpeg copies */
    av.at<int>(pkt_, av.off.pkt_size) = static_cast<int>(n);
    av.at<std::int64_t>(pkt_, av.off.pkt_pts) = rescale(pts_ms, tb.den, static_cast<std::int64_t>(1000) * tb.num);
    av.at<std::int64_t>(pkt_, av.off.pkt_dts) = av.at<std::int64_t>(pkt_, av.off.pkt_pts);
    av.at<std::int64_t>(pkt_, av.off.pkt_duration) = rescale(dur_ms, tb.den, static_cast<std::int64_t>(1000) * tb.num);
    av.at<int>(pkt_, av.off.pkt_stream_index) = stream;
    av.at<int>(pkt_, av.off.pkt_flags) = key ? av::kPktKey : 0;
    const int r = av.interleaved_write_frame(fmt_, pkt_);
    if (r >= 0) bytes_ += n;
    return r;
}

int MkvMuxer::write_video(const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms, bool key)
{
    const int r = write(0, data, n, pts_ms, dur_ms, key);
    if (r >= 0) ++video_packets_;
    return r;
}

int MkvMuxer::write_audio(const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms)
{
    if (audio_index_ < 0) return 0;
    return write(audio_index_, data, n, pts_ms, dur_ms, true);
}

void MkvMuxer::flush()
{
    if (fmt_ && *av_->fmt_pb(fmt_)) av_->avio_flush(*av_->fmt_pb(fmt_));
}

int MkvMuxer::close(std::string *err)
{
    if (!fmt_) return 0;
    int r = 0;
    if (header_) {
        r = av_->write_trailer(fmt_);
        if (r < 0 && err) *err = "trailer: " + av_->error_text(r);
    }
    if (*av_->fmt_pb(fmt_)) av_->avio_closep(av_->fmt_pb(fmt_));
    av_->free_context(fmt_);
    fmt_ = nullptr;
    if (pkt_) av_->packet_free(&pkt_);
    header_ = false;
    return r;
}

/* ----------------------------------------------------------------------------------------------------- the repair */

RemuxResult remux_mkv(const AvLibs &av, const std::string &in, const std::string &out)
{
    RemuxResult res;
    av::FormatContext *ic = nullptr;
    int r = av.open_input(&ic, in.c_str(), nullptr, nullptr);
    if (r < 0) {
        res.error = "open: " + av.error_text(r);
        return res;
    }
    r = av.find_stream_info(ic, nullptr);
    if (r < 0) {
        av.close_input(&ic);
        res.error = "streams: " + av.error_text(r);
        return res;
    }
    av::FormatContext *oc = nullptr;
    r = av.alloc_output_context2(&oc, nullptr, "matroska", out.c_str());
    if (r < 0 || !oc) {
        av.close_input(&ic);
        res.error = "matroska: " + av.error_text(r);
        return res;
    }
    const unsigned n = av.fmt_nb_streams(ic);
    std::vector<int> video(n, 0);
    std::vector<av::Rational> in_tb(n), out_tb(n);
    bool ok = n > 0 && n <= 8;
    for (unsigned i = 0; ok && i < n; ++i) {
        av::Stream *is = av.fmt_stream(ic, i);
        av::Stream *os = av.new_stream(oc, nullptr);
        if (!os || av.parameters_copy(av.stream_par(os), av.stream_par(is)) < 0) ok = false;
        if (!ok) break;
        video[i] = av.at<int>(av.stream_par(is), av.off.par_codec_type) == av::kMediaVideo;
        in_tb[i] = av.stream_time_base(is);
        av.stream_time_base(os) = in_tb[i];
    }
    if (ok) ok = av.avio_open(av.fmt_pb(oc), out.c_str(), av::kIoWrite) >= 0;
    av::Dictionary *opts = nullptr;
    av.dict_set(&opts, "cluster_time_limit", "1000", 0);
    if (ok) ok = av.write_header(oc, &opts) >= 0;
    av.dict_free(&opts);
    if (!ok) {
        res.error = "cannot start the repaired file";
        if (*av.fmt_pb(oc)) av.avio_closep(av.fmt_pb(oc));
        av.free_context(oc);
        av.close_input(&ic);
        return res;
    }
    for (unsigned i = 0; i < n; ++i) out_tb[i] = av.stream_time_base(av.fmt_stream(oc, i));
    av::Packet *pkt = av.packet_alloc();
    for (;;) {
        r = av.read_frame(ic, pkt);
        if (r < 0) {
            res.stopped_at_error = r != av::kErrEof;
            break;
        }
        const int si = av.at<int>(pkt, av.off.pkt_stream_index);
        if (si < 0 || static_cast<unsigned>(si) >= n) {
            av.packet_unref(pkt);
            continue;
        }
        const av::Rational a = in_tb[static_cast<std::size_t>(si)], b = out_tb[static_cast<std::size_t>(si)];
        std::int64_t &pts = av.at<std::int64_t>(pkt, av.off.pkt_pts);
        std::int64_t &dts = av.at<std::int64_t>(pkt, av.off.pkt_dts);
        std::int64_t &dur = av.at<std::int64_t>(pkt, av.off.pkt_duration);
        if (pts != av::kNoPts) pts = rescale(pts, static_cast<std::int64_t>(a.num) * b.den, static_cast<std::int64_t>(a.den) * b.num);
        if (dts != av::kNoPts) dts = rescale(dts, static_cast<std::int64_t>(a.num) * b.den, static_cast<std::int64_t>(a.den) * b.num);
        dur = rescale(dur, static_cast<std::int64_t>(a.num) * b.den, static_cast<std::int64_t>(a.den) * b.num);
        if (video[static_cast<std::size_t>(si)]) {
            ++res.video_packets;
            if (pts != av::kNoPts)
                res.duration_ms = std::max(res.duration_ms, rescale(pts + dur, static_cast<std::int64_t>(b.num) * 1000, b.den));
        } else {
            ++res.audio_packets;
        }
        if (av.interleaved_write_frame(oc, pkt) < 0) {
            av.packet_unref(pkt);
            break;
        }
    }
    av.packet_free(&pkt);
    r = av.write_trailer(oc);
    av.avio_closep(av.fmt_pb(oc));
    av.free_context(oc);
    av.close_input(&ic);
    res.ok = r >= 0 && res.video_packets > 0;
    if (!res.ok && res.error.empty()) res.error = res.video_packets ? "trailer: " + av.error_text(r) : "no video packet";
    return res;
}

} // namespace vhs
