/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl decoders: one music file as interleaved PCM frames (report 059, decision D2, option B+).
 *
 *   tier 1 (small libraries, package Depends): MP3 (libmpg123), FLAC (libFLAC through Cassette's FlacDecoder), Ogg Vorbis
 *          (libvorbisfile), Ogg Opus (libopus through Cassette's own Ogg reader), WAV and AIFF (Vinyl's own reader);
 *   tier 2 (FFmpeg, a Recommends, loaded only when a file needs it): AAC / ALAC / M4A, WMA, APE, WavPack, TTA, Musepack,
 *          DSF (as PCM), trackers, Ogg FLAC, and whatever else libavformat opens.
 *
 * The format is chosen by the file's magic bytes (an ID3 tag or an MPEG frame sync, "fLaC", "OggS" + the first packet,
 * "RIFF....WAVE", "FORM....AIFF"); anything else goes to FFmpeg. Without FFmpeg such a file fails with kNeedsFfmpeg,
 * whose notice says how to install it.
 *
 * Output: 16-bit samples (int16) for sources of 16 bits or fewer and the lossy formats, 32-bit samples (int32, the source
 * bits at the top) for 24-bit and 32-bit sources (decision D13: 24-bit is kept), at the source rate, at most 8 channels
 * (the engine plays the first two). Seeks are sample exact where the library is (mpg123 in its gapless timeline, libFLAC,
 * vorbisfile, Opus, WAV/AIFF); FFmpeg seeks to the packet at or before the frame. LVGL-free.
 */
#pragma once

#include "decode_libs.hpp"
#include "../codec/codec_libs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vinyl {

constexpr int kNeedsFfmpeg = -100300;  /* a tier-2 file and FFmpeg is not installed */
constexpr int kNeedsLibrary = -100301; /* a tier-1 file and its library is not installed */
constexpr int kNotAudio = -100302;     /* not a file Vinyl can play (unknown magic and no FFmpeg stream) */
constexpr int kDecodeFailed = -100303; /* the library failed on the file */

enum class Codec { Unknown, Mp3, Flac, Vorbis, Opus, Wav, Aiff, Ffmpeg };
const char *codec_name(Codec c); /* "MP3", "FLAC", "Vorbis", "Opus", "WAV", "AIFF", "FFmpeg" */

/* What read() delivers. */
struct PcmFormat {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 16; /* 16 (int16 samples) or 32 (int32 samples) */
    bool operator==(const PcmFormat &o) const { return rate == o.rate && channels == o.channels && bits == o.bits; }
    bool operator!=(const PcmFormat &o) const { return !(*this == o); }
    unsigned frame_bytes() const { return channels * (bits / 8u); }
};

struct TrackInfo {
    Codec codec = Codec::Unknown;
    std::string label;        /* the format's short name for the screen: "MP3", "FLAC", "AAC", "ALAC" ... */
    PcmFormat out;
    unsigned source_bits = 0; /* 16, 24, 32 for lossless and PCM sources; 0 for the lossy formats */
    uint64_t total_frames = 0;
    bool frames_known = false;
    bool lossy = false;
    unsigned bitrate_kbps = 0; /* lossy formats when known */
};

class TrackDecoder {
public:
    virtual ~TrackDecoder() = default;
    virtual int open(const std::string &path, TrackInfo &info) = 0;
    /* Up to `frames` frames into out (frames * info.out.frame_bytes() bytes): frames read, 0 at the end, or < 0. */
    virtual long read(void *out, size_t frames) = 0;
    virtual int seek(uint64_t frame) = 0;
    virtual uint64_t position() const = 0;
    virtual void close() = 0;
};

/* The libraries the decoders use; each getter is called only when a file of its format is opened (the FFmpeg getter maps
 * FFmpeg). A getter that returns null means "not installed". */
struct DecodeLibs {
    std::function<const Mpg123Lib *()> mpg123;
    std::function<const VorbisFileLib *()> vorbis;
    std::function<const FlacDecLib *()> flac;
    std::function<const OpusDecLib *()> opus;
    std::function<const FfmpegLib *()> ffmpeg;
};
DecodeLibs real_decode_libs();

/* The container by the first bytes of the file (no library). */
Codec probe_codec(const std::string &path, int *err = nullptr);

/* Opens a decoder for the file: the codec by probe_codec(), else FFmpeg. Null with *err set (kNeedsFfmpeg,
 * kNeedsLibrary, kNotAudio, kDecodeFailed, a codec error or -errno). */
std::unique_ptr<TrackDecoder> open_track(const std::string &path, const DecodeLibs &libs, TrackInfo &info, int *err);

/* The sentence a notice shows for an open error ("The FFmpeg libraries could not be loaded. Reinstall Vinyl." for
 * kNeedsFfmpeg: FFmpeg is in the package's Depends since decision D20, so this is a broken install, not a missing
 * option; the notice draft's wording "Install them from Settings, Apps" is dropped). */
std::string track_error_text(int err, const std::string &path);

/* Is the file name one the browser lists as a track (by extension, any case)? */
bool is_audio_name(const std::string &name);
/* Is it a playlist the browser opens as a queue (.m3u, .m3u8)? */
bool is_playlist_name(const std::string &name);

/* --------------------------------------------------------------------------- the readers, for the tests */

std::unique_ptr<TrackDecoder> make_mp3_decoder(const Mpg123Lib *lib);
std::unique_ptr<TrackDecoder> make_vorbis_decoder(const VorbisFileLib *lib);
std::unique_ptr<TrackDecoder> make_flac_decoder(const FlacDecLib *lib);
std::unique_ptr<TrackDecoder> make_opus_decoder(const OpusDecLib *lib);
std::unique_ptr<TrackDecoder> make_pcm_decoder(); /* WAV and AIFF */
std::unique_ptr<TrackDecoder> make_ffmpeg_decoder(const FfmpegLib *lib);

/* The codec label of an FFmpeg decoder name ("aac" -> "AAC", "alac" -> "ALAC", "wmav2" -> "WMA", "mp3float" -> "MP3"). */
std::string ffmpeg_codec_label(const std::string &decoder_name);

}  // namespace vinyl
