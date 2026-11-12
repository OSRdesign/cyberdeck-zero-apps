/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl decoders: tier 2 through FFmpeg (track_decoder.hpp; report 059 D2/D3), loaded at run time. libavformat opens and
 * demuxes the file, libavcodec decodes the best audio stream, libswresample turns its frames into interleaved int16 (lossy
 * codecs, lossless sources of 16 bits or fewer) or int32 (lossless sources of more than 16 bits) at the source rate.
 *
 * No FFmpeg header is used: the few struct fields read go through the offsets of decode_libs.hpp (checked against the
 * headers by tests/check_decode_abi.cpp), every other value through the AVOption API (av_opt_get_*), which is stable
 * across the major versions. Seeks land on the packet at or before the frame asked for (av_seek_frame backward).
 */
#include "track_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vinyl {

std::string ffmpeg_codec_label(const std::string &n) {
    struct Map {
        const char *prefix, *label;
    };
    static const Map map[] = {
        {"aac", "AAC"},     {"alac", "ALAC"},       {"wmalossless", "WMA"}, {"wmapro", "WMA"}, {"wmav", "WMA"},
        {"ape", "APE"},     {"wavpack", "WavPack"}, {"tta", "TTA"},         {"tak", "TAK"},    {"mpc", "Musepack"},
        {"musepack", "Musepack"}, {"dsd", "DSD"},   {"mp3", "MP3"},         {"mp2", "MP2"},    {"flac", "FLAC"},
        {"vorbis", "Vorbis"}, {"opus", "Opus"},     {"libopus", "Opus"},    {"ac3", "AC-3"},   {"eac3", "E-AC-3"},
        {"dts", "DTS"},     {"truehd", "TrueHD"},   {"pcm", "PCM"},         {"libopenmpt", "Tracker"},
        {"libgme", "Chiptune"}, {"speex", "Speex"},
    };
    for (const Map &m : map)
        if (n.compare(0, std::strlen(m.prefix), m.prefix) == 0) return m.label;
    std::string u = n;
    for (char &c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u.empty() ? "FFmpeg" : u;
}

namespace {

bool lossless_name(const std::string &n) {
    static const char *const names[] = {"alac", "flac", "ape", "wavpack", "tta", "tak", "mlp", "truehd", "wmalossless",
                                        "pcm", "dsd", "s302m"};
    for (const char *x : names)
        if (n.compare(0, std::strlen(x), x) == 0) return true;
    return false;
}

template <typename T> T field(const void *base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const char *>(base) + off, sizeof(T));
    return v;
}

class FfmpegDecoder final : public TrackDecoder {
public:
    explicit FfmpegDecoder(const FfmpegLib *lib) : L(lib) {}
    ~FfmpegDecoder() override { close(); }

    int open(const std::string &path, TrackInfo &info) override {
        close();
        if (!L || !L->complete()) return kNeedsFfmpeg;
        if (L->open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
            fmt_ = nullptr;
            return kNotAudio;
        }
        if (L->find_stream_info(fmt_, nullptr) < 0) {
            close();
            return kNotAudio;
        }
        const av::Codec *codec = nullptr;
        idx_ = L->find_best_stream(fmt_, av::kMediaAudio, -1, -1, &codec, 0);
        if (idx_ < 0 || !codec) {
            close();
            return kNotAudio;
        }
        const unsigned nb = field<unsigned>(fmt_, L->off.fmt_nb_streams);
        if (static_cast<unsigned>(idx_) >= nb) {
            close();
            return kDecodeFailed;
        }
        void *const *streams = field<void *const *>(fmt_, L->off.fmt_streams);
        const void *stream = streams ? streams[idx_] : nullptr;
        if (!stream) {
            close();
            return kDecodeFailed;
        }
        const void *par = field<const void *>(stream, L->off.stream_codecpar);
        tb_ = field<av::Rational>(stream, L->off.stream_time_base);
        const int64_t dur = field<int64_t>(stream, L->off.stream_duration);
        ctx_ = L->alloc_context(codec);
        if (!ctx_ || L->parameters_to_context(ctx_, par) < 0 || L->open2(ctx_, codec, nullptr) < 0) {
            std::fprintf(stderr, "[vinyl] FFmpeg: the decoder did not open for %s\n", path.c_str());
            close();
            return kDecodeFailed;
        }
        /* AVCodec starts with `const char *name` (stable since FFmpeg 1.x) */
        const char *cname = field<const char *>(codec, 0);
        const std::string decoder = cname ? cname : "";
        int64_t rate = 0, raw_bits = 0;
        av::ChannelLayout in{};
        const int r_ar = L->opt_get_int(ctx_, "ar", 0, &rate);
        const int r_ch = L->opt_get_chlayout(ctx_, "ch_layout", 0, &in);
        if (r_ar < 0 || rate <= 0 || r_ch < 0 || in.nb_channels < 1) {
            std::fprintf(stderr, "[vinyl] FFmpeg: no audio parameters for %s (ar %d=%lld, layout %d=%d)\n", path.c_str(), r_ar,
                         static_cast<long long>(rate), r_ch, in.nb_channels);
            close();
            return kDecodeFailed;
        }
        if (L->opt_get_int(ctx_, "bits_per_raw_sample", 0, &raw_bits) < 0) raw_bits = 0;
        lossless_ = lossless_name(decoder);
        raw_bits_ = static_cast<int>(raw_bits);
        channels_ = std::min(in.nb_channels, 8);
        rate_ = static_cast<uint32_t>(rate);
        in_layout_ = in;
        pkt_ = L->packet_alloc();
        frame_ = L->frame_alloc();
        if (!pkt_ || !frame_) {
            close();
            return kDecodeFailed;
        }
        /* the sample format is the decoder's own (no AVOption gives it): the first frame says it, and with it the output
         * size (int32 for lossless sources wider than 16 bits) and the resampler; that frame is kept for the first read */
        in_fmt_ = -1;
        const int first = next_frame();
        if (first <= 0 || !swr_) {
            std::fprintf(stderr, "[vinyl] FFmpeg: no first frame in %s (%d)\n", path.c_str(), first);
            close();
            return first < 0 ? first : kNotAudio;
        }
        const bool small = in_fmt_ == av::kU8 || in_fmt_ == av::kU8P || in_fmt_ == av::kS16 || in_fmt_ == av::kS16P;
        const bool lossless = lossless_;
        info = TrackInfo();
        info.codec = Codec::Ffmpeg;
        info.label = ffmpeg_codec_label(decoder);
        info.lossy = !lossless;
        info.source_bits = lossless ? static_cast<unsigned>(raw_bits ? raw_bits : (small ? 16 : 24)) : 0;
        info.out.rate = rate_;
        info.out.channels = static_cast<uint16_t>(channels_);
        info.out.bits = static_cast<uint16_t>(out_bits_);
        if (dur != av::kNoPts && dur > 0 && tb_.den > 0 && tb_.num > 0) {
            info.total_frames = static_cast<uint64_t>(static_cast<long double>(dur) * tb_.num * rate_ / tb_.den);
            info.frames_known = true;
        }
        pos_ = 0;
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (!ctx_) return -EBADF;
        unsigned char *o = static_cast<unsigned char *>(out);
        size_t done = 0;
        while (done < frames) {
            const size_t have = (buf_.size() - buf_off_) / fb_;
            if (have > 0) {
                const size_t take = std::min(have, frames - done);
                std::memcpy(o + done * fb_, buf_.data() + buf_off_, take * fb_);
                buf_off_ += take * fb_;
                done += take;
                continue;
            }
            buf_.clear();
            buf_off_ = 0;
            if (finished_) break;
            const int r = next_frame();
            if (r < 0) {
                if (done) break;
                return r;
            }
            if (r == 0) finished_ = true;
        }
        pos_ += done;
        return static_cast<long>(done);
    }

    int seek(uint64_t frame) override {
        if (!ctx_) return -EBADF;
        int64_t ts = 0;
        if (tb_.num > 0 && tb_.den > 0 && rate_ > 0)
            ts = static_cast<int64_t>(static_cast<long double>(frame) * tb_.den / (static_cast<long double>(tb_.num) * rate_));
        if (L->seek_frame(fmt_, idx_, ts, av::kSeekBackward) < 0 && frame != 0) return kDecodeFailed;
        L->flush_buffers(ctx_);
        buf_.clear();
        buf_off_ = 0;
        finished_ = eof_sent_ = false;
        pos_ = frame;
        return 0;
    }

    uint64_t position() const override { return pos_; }

    void close() override {
        if (!L) return;
        if (frame_) L->frame_free(&frame_);
        if (pkt_) L->packet_free(&pkt_);
        if (swr_) L->swr_free(&swr_);
        if (ctx_) L->free_context(&ctx_);
        if (fmt_) L->close_input(&fmt_);
        if (in_layout_.nb_channels) L->chlayout_uninit(&in_layout_);
        in_layout_ = av::ChannelLayout{};
        frame_ = nullptr;
        pkt_ = nullptr;
        swr_ = nullptr;
        ctx_ = nullptr;
        fmt_ = nullptr;
        buf_.clear();
        buf_off_ = 0;
        finished_ = eof_sent_ = false;
    }

private:
    bool make_swr() {
        if (swr_) L->swr_free(&swr_);
        av::ChannelLayout outl{};
        L->chlayout_default(&outl, channels_);
        const int ofmt = out_bits_ == 32 ? av::kS32 : av::kS16;
        const int r = L->swr_alloc_set_opts2(&swr_, &outl, ofmt, static_cast<int>(rate_), &in_layout_, in_fmt_,
                                             static_cast<int>(rate_), 0, nullptr);
        L->chlayout_uninit(&outl);
        if (r < 0 || !swr_ || L->swr_init(swr_) < 0) return false;
        return true;
    }

    /* Decodes one more frame into buf_: 1, 0 at the end of the stream, < 0 on an error. */
    int next_frame() {
        for (int guard = 0; guard < 10000; ++guard) {
            const int r = L->receive_frame(ctx_, frame_);
            if (r == 0) {
                const int ffmt = field<int>(frame_, L->off.frame_format);
                if (ffmt != in_fmt_ && ffmt >= 0) {
                    if (in_fmt_ < 0) { /* the first frame: the output size follows the source */
                        const bool small = ffmt == av::kU8 || ffmt == av::kU8P || ffmt == av::kS16 || ffmt == av::kS16P;
                        out_bits_ = (lossless_ && !small && (raw_bits_ == 0 || raw_bits_ > 16)) ? 32 : 16;
                        fb_ = static_cast<size_t>(channels_) * (out_bits_ / 8);
                    }
                    in_fmt_ = ffmt;
                    if (!make_swr()) return kDecodeFailed;
                }
                const int n = field<int>(frame_, L->off.frame_nb_samples);
                const uint8_t **in = field<const uint8_t **>(frame_, L->off.frame_extended_data);
                if (n > 0 && in) {
                    const int cap = n + 256;
                    buf_.resize(static_cast<size_t>(cap) * fb_);
                    uint8_t *outp = buf_.data();
                    const int got = L->swr_convert(swr_, &outp, cap, in, n);
                    buf_.resize(got > 0 ? static_cast<size_t>(got) * fb_ : 0);
                }
                L->frame_unref(frame_);
                if (!buf_.empty()) return 1;
                continue;
            }
            if (r == av::kErrEof) return 0;
            if (r != av::kErrEagain) return kDecodeFailed;
            if (eof_sent_) return 0;
            const int rr = L->read_frame(fmt_, pkt_);
            if (rr < 0) {
                L->send_packet(ctx_, nullptr); /* drain the decoder */
                eof_sent_ = true;
                continue;
            }
            if (field<int>(pkt_, L->off.packet_stream_index) == idx_) L->send_packet(ctx_, pkt_);
            L->packet_unref(pkt_);
        }
        return kDecodeFailed;
    }

    const FfmpegLib *L;
    av::FormatContext *fmt_ = nullptr;
    av::CodecContext *ctx_ = nullptr;
    av::Swr *swr_ = nullptr;
    av::Packet *pkt_ = nullptr;
    av::Frame *frame_ = nullptr;
    av::ChannelLayout in_layout_{};
    av::Rational tb_{0, 1};
    int idx_ = -1;
    int channels_ = 2, in_fmt_ = -1, out_bits_ = 16, raw_bits_ = 0;
    bool lossless_ = false;
    uint32_t rate_ = 0;
    size_t fb_ = 4;
    std::vector<uint8_t> buf_;
    size_t buf_off_ = 0;
    bool finished_ = false, eof_sent_ = false;
    uint64_t pos_ = 0;
};

}  // namespace

std::unique_ptr<TrackDecoder> make_ffmpeg_decoder(const FfmpegLib *lib) { return std::make_unique<FfmpegDecoder>(lib); }

}  // namespace vinyl
