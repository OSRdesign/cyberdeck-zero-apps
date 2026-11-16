/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Vinyl 0.3.0 (apps/vinyl at fdad829) for VHS (brief 3.1, 10); VHS changes are marked VHS.
 *
 * Vinyl decoders: tier 2 through FFmpeg (track_decoder.hpp; report 059 D2/D3), loaded at run time. libavformat opens and
 * demuxes the file, libavcodec decodes the best audio stream, libswresample turns its frames into interleaved int16 (lossy
 * codecs, lossless sources of 16 bits or fewer) or int32 (lossless sources of more than 16 bits) at the source rate.
 *
 * No FFmpeg header is used: the few struct fields read go through the offsets of decode_libs.hpp (checked against the
 * headers by tests/check_decode_abi.cpp), every other value through the AVOption API (av_opt_get_*), which is stable
 * across the major versions. Seeks land on the packet at or before the frame asked for (av_seek_frame backward).
 *
 * VHS changes:
 *   - the nested namespace is dav (record/av_libs.hpp has vhs::av);
 *   - a file that opens but has no audio stream is kNoAudioStream (a video-only scene), not kNotAudio; a file that cannot
 *     be opened keeps libavformat's -errno (ENOENT, EACCES);
 *   - the position is movie time: 0 is the container's start_time when that is positive (a foreign MP4 that starts at
 *     1.4 s), else 0 (an MKV whose AAC priming makes start_time -64 ms: the pre-roll before 0 is dropped, so the first sample
 *     delivered is 0's and a video's pts 0 meets it); the total falls back to the container's length when the audio stream
 *     has none (an MKV's audio stream usually has none);
 *   - a seek goes through the container's default stream (the video when there is one: a scene's MKV has cues for its
 *     video only, an audio-stream seek would scan the whole file), then the first packet of the audio stream says where
 *     the demuxer really landed (a keyframe before the target) and the decoder drops the samples up to the target, so
 *     position() is the target (or, past it, where the audio really starts) and the engine's heard clock stays true.
 */
#include "track_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vhs {

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
        const int opened = L->open_input(&fmt_, path.c_str(), nullptr, nullptr);
        if (opened < 0) {
            fmt_ = nullptr;
            return opened > -4096 ? opened : kNotAudio; /* VHS: -ENOENT and -EACCES stay what they are */
        }
        if (L->find_stream_info(fmt_, nullptr) < 0) {
            close();
            return kNotAudio;
        }
        const dav::Codec *codec = nullptr;
        idx_ = L->find_best_stream(fmt_, dav::kMediaAudio, -1, -1, &codec, 0);
        if (idx_ < 0 || !codec) {
            const bool no_stream = idx_ == dav::kErrStreamNotFound; /* VHS: a video-only file is not an error */
            close();
            return no_stream ? kNoAudioStream : kDecodeFailed;
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
        tb_ = field<dav::Rational>(stream, L->off.stream_time_base);
        const int64_t dur = field<int64_t>(stream, L->off.stream_duration);
        /* VHS: the container's origin and length (AV_TIME_BASE units) */
        const int64_t start_us = field<int64_t>(fmt_, L->off.fmt_start_time);
        const int64_t fmt_dur_us = field<int64_t>(fmt_, L->off.fmt_duration);
        origin_us_ = start_us != dav::kNoPts && start_us > 0 ? start_us : 0;
        ctx_ = L->alloc_context(codec);
        if (!ctx_ || L->parameters_to_context(ctx_, par) < 0 || L->open2(ctx_, codec, nullptr) < 0) {
            std::fprintf(stderr, "[vhs] FFmpeg: the decoder did not open for %s\n", path.c_str());
            close();
            return kDecodeFailed;
        }
        /* AVCodec starts with `const char *name` (stable since FFmpeg 1.x) */
        const char *cname = field<const char *>(codec, 0);
        const std::string decoder = cname ? cname : "";
        int64_t rate = 0, raw_bits = 0;
        dav::ChannelLayout in{};
        const int r_ar = L->opt_get_int(ctx_, "ar", 0, &rate);
        const int r_ch = L->opt_get_chlayout(ctx_, "ch_layout", 0, &in);
        if (r_ar < 0 || rate <= 0 || r_ch < 0 || in.nb_channels < 1) {
            std::fprintf(stderr, "[vhs] FFmpeg: no audio parameters for %s (ar %d=%lld, layout %d=%d)\n", path.c_str(), r_ar,
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
        first_sent_valid_ = false;
        first_sent_pts_ = dav::kNoPts;
        const int first = next_frame();
        if (first <= 0 || !swr_) {
            std::fprintf(stderr, "[vhs] FFmpeg: no first frame in %s (%d)\n", path.c_str(), first);
            close();
            return first < 0 ? first : kNotAudio;
        }
        const bool small = in_fmt_ == dav::kU8 || in_fmt_ == dav::kU8P || in_fmt_ == dav::kS16 || in_fmt_ == dav::kS16P;
        const bool lossless = lossless_;
        info = TrackInfo();
        info.codec = Codec::Ffmpeg;
        info.label = ffmpeg_codec_label(decoder);
        info.lossy = !lossless;
        info.source_bits = lossless ? static_cast<unsigned>(raw_bits ? raw_bits : (small ? 16 : 24)) : 0;
        info.out.rate = rate_;
        info.out.channels = static_cast<uint16_t>(channels_);
        info.out.bits = static_cast<uint16_t>(out_bits_);
        if (dur != dav::kNoPts && dur > 0 && tb_.den > 0 && tb_.num > 0) {
            info.total_frames = static_cast<uint64_t>(static_cast<long double>(dur) * tb_.num * rate_ / tb_.den);
            info.frames_known = true;
        } else if (fmt_dur_us != dav::kNoPts && fmt_dur_us > 0) { /* VHS: the container's length, from the origin */
            const int64_t end_us = (start_us != dav::kNoPts ? start_us : 0) + fmt_dur_us;
            if (end_us > origin_us_) {
                info.total_frames = static_cast<uint64_t>(static_cast<long double>(end_us - origin_us_) * rate_ / 1000000.0L);
                info.frames_known = true;
            }
        }
        pos_ = 0;
        skip_ = 0;
        settle(-1);
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (!ctx_) return -EBADF;
        unsigned char *o = static_cast<unsigned char *>(out);
        size_t done = 0;
        while (done < frames) {
            const size_t have = (buf_.size() - buf_off_) / fb_;
            if (have > 0 && skip_ > 0) { /* VHS: the samples between where a seek landed and its target */
                const size_t drop = static_cast<size_t>(std::min<uint64_t>(have, skip_));
                buf_off_ += drop * fb_;
                skip_ -= drop;
                continue;
            }
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
        /* VHS: through the container's default stream (stream -1, AV_TIME_BASE units): the video when there is one, whose
         * index (a Matroska scene has cues for its video only) finds the place at once; the audio stream's own seek would
         * read the file from its start */
        /* it aims a quarter of a second before the target, so the landing is before it (the index's own place can be after
         * the audio packet that holds the target) and the samples up to the target are dropped: the position is exact */
        const uint64_t aim = rate_ > 0 && frame > rate_ / 4 ? frame - rate_ / 4 : 0;
        int64_t ts = origin_us_;
        if (rate_ > 0) ts += static_cast<int64_t>(static_cast<long double>(aim) * 1000000.0L / rate_);
        const int sr = L->seek_frame(fmt_, -1, ts, dav::kSeekBackward);
        if (sr < 0 && frame != 0) return kDecodeFailed;
        L->flush_buffers(ctx_);
        buf_.clear();
        buf_off_ = 0;
        finished_ = eof_sent_ = false;
        pos_ = frame;
        skip_ = 0;
        first_sent_valid_ = false;
        first_sent_pts_ = dav::kNoPts;
        if (sr >= 0) landed(frame);
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
        in_layout_ = dav::ChannelLayout{};
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
    /* VHS: after a seek the first packet of the audio stream says where the demuxer really landed (a keyframe before the
     * target, at a keyframe's own granularity); the packet goes to the decoder. */
    void landed(uint64_t target) {
        for (int guard = 0; guard < 100000; ++guard) {
            if (L->read_frame(fmt_, pkt_) < 0) {
                L->send_packet(ctx_, nullptr); /* the end of the file: drain */
                eof_sent_ = true;
                return;
            }
            if (field<int>(pkt_, L->off.packet_stream_index) != idx_) {
                L->packet_unref(pkt_);
                continue;
            }
            first_sent_pts_ = field<int64_t>(pkt_, L->off.packet_pts);
            first_sent_valid_ = true;
            L->send_packet(ctx_, pkt_);
            L->packet_unref(pkt_);
            settle(static_cast<int64_t>(target));
            return;
        }
    }

    /* VHS: where the first sample the decoder will deliver lies (the first packet sent since the open or the seek, in frames
     * from the origin; negative for AAC's priming), and so what position() is. target < 0: just opened: the samples before
     * the origin are dropped and position() is the first one's place. A seek to `target`: landed before it, the samples up
     * to it are dropped by read() (at most 30 s of them: a longer gap keeps the place the demuxer found), so the first
     * sample delivered is the target's; landed after it (the audio starts later), position() is that place. */
    void settle(int64_t target) {
        constexpr int64_t kMaxSkipSeconds = 30;
        if (!first_sent_valid_ || first_sent_pts_ == dav::kNoPts || tb_.num <= 0 || tb_.den <= 0 || rate_ == 0) return;
        const long double sec =
            static_cast<long double>(first_sent_pts_) * tb_.num / tb_.den - static_cast<long double>(origin_us_) / 1000000.0L;
        const int64_t first = static_cast<int64_t>(std::llround(sec * rate_)); /* the first sample's frame */
        if (target < 0) {
            if (first < 0) {
                skip_ = static_cast<uint64_t>(-first);
                pos_ = 0;
            } else {
                pos_ = static_cast<uint64_t>(first);
            }
            return;
        }
        if (first >= target) {
            pos_ = static_cast<uint64_t>(first);
        } else if (target - first <= kMaxSkipSeconds * static_cast<int64_t>(rate_)) {
            skip_ = static_cast<uint64_t>(target - first);
            pos_ = static_cast<uint64_t>(target);
        } else if (first < 0) {
            skip_ = static_cast<uint64_t>(-first);
            pos_ = 0;
        } else {
            pos_ = static_cast<uint64_t>(first);
        }
    }

    bool make_swr() {
        if (swr_) L->swr_free(&swr_);
        dav::ChannelLayout outl{};
        L->chlayout_default(&outl, channels_);
        const int ofmt = out_bits_ == 32 ? dav::kS32 : dav::kS16;
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
                        const bool small = ffmt == dav::kU8 || ffmt == dav::kU8P || ffmt == dav::kS16 || ffmt == dav::kS16P;
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
            if (r == dav::kErrEof) return 0;
            if (r != dav::kErrEagain) return kDecodeFailed;
            if (eof_sent_) return 0;
            const int rr = L->read_frame(fmt_, pkt_);
            if (rr < 0) {
                L->send_packet(ctx_, nullptr); /* drain the decoder */
                eof_sent_ = true;
                continue;
            }
            if (field<int>(pkt_, L->off.packet_stream_index) == idx_) {
                if (!first_sent_valid_) { /* VHS: where the stream's first sample lies, for settle() */
                    first_sent_pts_ = field<int64_t>(pkt_, L->off.packet_pts);
                    first_sent_valid_ = true;
                }
                L->send_packet(ctx_, pkt_);
            }
            L->packet_unref(pkt_);
        }
        return kDecodeFailed;
    }

    const FfmpegLib *L;
    dav::FormatContext *fmt_ = nullptr;
    dav::CodecContext *ctx_ = nullptr;
    dav::Swr *swr_ = nullptr;
    dav::Packet *pkt_ = nullptr;
    dav::Frame *frame_ = nullptr;
    dav::ChannelLayout in_layout_{};
    dav::Rational tb_{0, 1};
    int idx_ = -1;
    int channels_ = 2, in_fmt_ = -1, out_bits_ = 16, raw_bits_ = 0;
    bool lossless_ = false;
    uint32_t rate_ = 0;
    size_t fb_ = 4;
    std::vector<uint8_t> buf_;
    size_t buf_off_ = 0;
    bool finished_ = false, eof_sent_ = false;
    uint64_t pos_ = 0;
    int64_t origin_us_ = 0; /* VHS: movie time 0, in AV_TIME_BASE units (the container's start_time when positive) */
    uint64_t skip_ = 0;     /* VHS: frames to drop (pre-roll before the origin, or after a seek that landed before its target) */
    bool first_sent_valid_ = false;
    int64_t first_sent_pts_ = dav::kNoPts; /* VHS: the pts of the first packet sent to the decoder since the open or seek */
};

}  // namespace

std::unique_ptr<TrackDecoder> make_ffmpeg_decoder(const FfmpegLib *lib) { return std::make_unique<FfmpegDecoder>(lib); }

}  // namespace vhs
