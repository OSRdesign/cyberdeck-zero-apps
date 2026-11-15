/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the MKV file and the AAC encoder (brief 6.1, 6.3, 6.5; D8). Used by the recorder's writer thread (and by the
 * repair on the scenes worker); one owner per context (brief 3.3 rule 1). LVGL-free.
 *
 *   MkvMuxer    one MKV part: a video stream whose packets are written as they come (P1: the camera's MJPEG frames,
 *               V_MJPEG, copied unchanged; the VideoSink interface takes any codec's packets, H.264 is P2) and an
 *               optional AAC stream; timestamps in ms (the stream time base 1/1000); the muxer option
 *               cluster_time_limit 1000 ms, so a crash loses about 1 s at most
 *   AacEncoder  FFmpeg's native "aac" (AAC-LC): S16 interleaved in, converted to float planar by own code (no
 *               libswresample), 64 kbit/s at 24 kHz and below, 128 kbit/s above; packets with their pts in samples
 *   remux_mkv() the repair (brief 6.3): copies the packets of a damaged file until the first read error into a new file
 */
#pragma once

#include "av_libs.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace vhs {

enum class VideoCodec { Mjpeg, H264 };
const char *video_codec_name(VideoCodec c); /* "MJPEG", "H.264" */

struct VideoFormat {
    VideoCodec codec = VideoCodec::Mjpeg;
    int w = 0, h = 0;
    int fps = 30;
};

/* Where the recorder puts video packets: it never looks inside them (brief 4.3: the interface must not assume MJPEG). */
class VideoSink {
public:
    virtual ~VideoSink() = default;
    /* 0 or a negative error. pts / duration in ms from the part's start. */
    virtual int write_video(const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms, bool key) = 0;
};

struct EncodedAudio {
    std::vector<std::uint8_t> data;
    std::int64_t pts = 0;      /* samples (may start below 0: the encoder's priming) */
    std::int64_t duration = 0; /* samples */
};

class AacEncoder {
public:
    AacEncoder() = default;
    ~AacEncoder();
    AacEncoder(const AacEncoder &) = delete;
    AacEncoder &operator=(const AacEncoder &) = delete;

    bool open(const AvLibs &av, int rate, int channels, std::string *err);
    /* S16 interleaved frames: buffered into the encoder's frames (1024 samples); packets queue up for pop(). */
    int push(const std::int16_t *interleaved, std::size_t frames);
    /* The end: the last partial frame and the encoder's tail. */
    int finish();
    bool pop(EncodedAudio &out);
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    int frame_size() const { return frame_size_; }
    std::int64_t samples_in() const { return samples_in_; }
    av::CodecContext *context() const { return ctx_; }
    static int bit_rate_for(int rate) { return rate <= 24000 ? 64000 : 128000; }

private:
    int send_current(int nb);
    int drain_packets();
    const AvLibs *av_ = nullptr;
    av::CodecContext *ctx_ = nullptr;
    av::Frame *frame_ = nullptr;
    av::Packet *pkt_ = nullptr;
    int rate_ = 0, channels_ = 0, frame_size_ = 1024;
    int fill_ = 0;              /* samples in the frame being filled */
    std::int64_t next_pts_ = 0; /* the pts (samples) of the frame being filled */
    std::int64_t samples_in_ = 0;
    std::deque<EncodedAudio> out_;
    bool finished_ = false;
};

class MkvMuxer : public VideoSink {
public:
    MkvMuxer() = default;
    ~MkvMuxer() override;
    MkvMuxer(const MkvMuxer &) = delete;
    MkvMuxer &operator=(const MkvMuxer &) = delete;

    /* audio: the encoder whose parameters the AAC stream takes (null: video only). */
    bool open(const AvLibs &av, const std::string &path, const VideoFormat &v, const AacEncoder *audio, std::string *err);
    int write_video(const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms, bool key) override;
    int write_audio(const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms);
    /* avio_flush: what the muxer wrote reaches the file (the caller fdatasyncs it). */
    void flush();
    /* The trailer (cues, sizes), then the file is closed. */
    int close(std::string *err);
    bool is_open() const { return fmt_ != nullptr; }
    bool has_audio() const { return audio_index_ >= 0; }
    std::uint64_t bytes() const { return bytes_; } /* packet bytes written */
    std::uint64_t video_packets() const { return video_packets_; }

private:
    int write(int stream, const std::uint8_t *data, std::size_t n, std::int64_t pts_ms, std::int64_t dur_ms, bool key);
    const AvLibs *av_ = nullptr;
    av::FormatContext *fmt_ = nullptr;
    av::Packet *pkt_ = nullptr;
    int audio_index_ = -1;
    av::Rational tb_[2] = {{1, 1000}, {1, 1000}};
    std::uint64_t bytes_ = 0, video_packets_ = 0;
    bool header_ = false;
};

struct RemuxResult {
    bool ok = false;               /* a file was written with at least one video packet */
    std::uint64_t video_packets = 0, audio_packets = 0;
    std::int64_t duration_ms = 0;  /* the last video packet's end */
    bool stopped_at_error = false; /* the copy ended at a read error (the damage), not at a clean end */
    std::string error;
};

/* Copies the packets of `in` into a new MKV `out` until the first read error (brief 6.3 step 1). */
RemuxResult remux_mkv(const AvLibs &av, const std::string &in, const std::string &out);

/* x * num / den rounded to the nearest, without overflow for the values VHS uses. */
std::int64_t rescale(std::int64_t x, std::int64_t num, std::int64_t den);

/* S16 interleaved -> float planar, own code (brief 6.1: no libswresample). planes[c] holds `frames` floats. */
void s16_to_float_planar(const std::int16_t *in, std::size_t frames, int channels, float *const *planes);

} // namespace vhs
