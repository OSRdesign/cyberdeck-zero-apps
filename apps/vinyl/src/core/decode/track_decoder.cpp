/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl decoders: the probe, the factory and the tier-1 readers (see track_decoder.hpp). FFmpeg is in ffmpeg_decoder.cpp.
 */
#include "track_decoder.hpp"

#include "../codec/decoder.hpp"
#include "../codec/flac_decoder.hpp"
#include "../codec/opus_decoder.hpp"
#include "../fileops.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace vinyl {

const char *codec_name(Codec c) {
    switch (c) {
    case Codec::Mp3: return "MP3";
    case Codec::Flac: return "FLAC";
    case Codec::Vorbis: return "Vorbis";
    case Codec::Opus: return "Opus";
    case Codec::Wav: return "WAV";
    case Codec::Aiff: return "AIFF";
    case Codec::Ffmpeg: return "FFmpeg";
    default: return "?";
    }
}

namespace {

std::string lower_ext(const std::string &name) {
    const size_t dot = name.rfind('.');
    const size_t slash = name.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = name.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

ssize_t pread_full(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        const ssize_t k = ::pread(fd, static_cast<char *>(buf) + got, n - got, static_cast<off_t>(off + got));
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) return -errno;
        if (k == 0) break;
        got += static_cast<size_t>(k);
    }
    return static_cast<ssize_t>(got);
}

/* An MPEG audio frame header at p (layer I-III, a valid bitrate and rate index). */
bool mpeg_sync(const unsigned char *p) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    if (((p[1] >> 3) & 3) == 1) return false;   /* reserved version */
    if (((p[1] >> 1) & 3) == 0) return false;   /* reserved layer */
    if ((p[2] >> 4) == 15) return false;        /* bad bitrate */
    if (((p[2] >> 2) & 3) == 3) return false;   /* reserved rate */
    return true;
}

}  // namespace

bool is_audio_name(const std::string &name) {
    static const char *const exts[] = {"mp3", "mp2", "flac", "ogg", "oga", "opus", "wav", "wave", "aif", "aiff", "aifc",
                                       "m4a", "m4b", "mp4", "aac", "alac", "wma", "ape", "wv", "tta", "mpc", "dsf",
                                       "dff", "mod", "xm", "s3m", "it", "mka", "webm", "ac3"};
    const std::string e = lower_ext(name);
    if (e.empty()) return false;
    for (const char *x : exts)
        if (e == x) return true;
    return false;
}

bool is_playlist_name(const std::string &name) {
    const std::string e = lower_ext(name);
    return e == "m3u" || e == "m3u8";
}

Codec probe_codec(const std::string &path, int *err) {
    if (err) *err = 0;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = -errno;
        return Codec::Unknown;
    }
    unsigned char h[64] = {0};
    const ssize_t n = pread_full(fd, h, sizeof h, 0);
    Codec c = Codec::Unknown;
    if (n >= 12 && std::memcmp(h, "fLaC", 4) == 0) {
        c = Codec::Flac;
    } else if (n >= 36 && std::memcmp(h, "OggS", 4) == 0) {
        const unsigned segs = h[26];
        const size_t body = 27 + segs;
        unsigned char p[16] = {0};
        if (pread_full(fd, p, sizeof p, body) >= 8) {
            if (p[0] == 0x01 && std::memcmp(p + 1, "vorbis", 6) == 0) c = Codec::Vorbis;
            else if (std::memcmp(p, "OpusHead", 8) == 0) c = Codec::Opus;
            else c = Codec::Ffmpeg; /* Ogg FLAC, Speex, Theora...: FFmpeg's */
        }
    } else if (n >= 12 && (std::memcmp(h, "RIFF", 4) == 0 || std::memcmp(h, "RF64", 4) == 0 ||
                           std::memcmp(h, "BW64", 4) == 0) &&
               std::memcmp(h + 8, "WAVE", 4) == 0) {
        c = Codec::Wav;
    } else if (n >= 12 && std::memcmp(h, "FORM", 4) == 0 &&
               (std::memcmp(h + 8, "AIFF", 4) == 0 || std::memcmp(h + 8, "AIFC", 4) == 0)) {
        c = Codec::Aiff;
    } else if (n >= 10 && std::memcmp(h, "ID3", 3) == 0) {
        /* an ID3v2 tag: MP3 when an MPEG frame follows it, else (a tagged AAC/ADTS or FLAC file) look past the tag */
        const uint64_t sz = (static_cast<uint64_t>(h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) |
                            (h[9] & 0x7F);
        uint64_t off = 10 + sz + ((h[5] & 0x10) ? 10 : 0);
        unsigned char f[4] = {0};
        bool found = false;
        for (int skip = 0; skip < 4096 && !found; ++skip) { /* padding of zeros may follow a tag */
            if (pread_full(fd, f, 4, off + skip) != 4) break;
            if (mpeg_sync(f)) found = true;
            else if (std::memcmp(f, "fLaC", 4) == 0) {
                c = Codec::Flac;
                break;
            } else if (f[0] != 0) {
                break;
            }
        }
        if (found) c = Codec::Mp3;
        else if (c == Codec::Unknown) c = Codec::Ffmpeg;
    } else if (n >= 4 && mpeg_sync(h)) {
        /* an ADTS AAC header also starts with 0xFFF: layer bits 00 there, which mpeg_sync refuses */
        c = Codec::Mp3;
    } else if (n > 0) {
        c = Codec::Ffmpeg;
    } else if (err) {
        *err = kNotAudio;
    }
    ::close(fd);
    return c;
}

/* ------------------------------------------------------------------------------------------------ MP3 (libmpg123) */

namespace {

class Mp3Decoder final : public TrackDecoder {
public:
    explicit Mp3Decoder(const Mpg123Lib *lib) : lib_(lib) {}
    ~Mp3Decoder() override { close(); }

    int open(const std::string &path, TrackInfo &info) override {
        close();
        if (!lib_ || !lib_->complete()) return kNeedsLibrary;
        int e = 0;
        mh_ = lib_->create(nullptr, &e);
        if (!mh_) return kDecodeFailed;
        lib_->param(mh_, mpg::kParamAddFlags, mpg::kFlagQuiet | mpg::kFlagGapless, 0);
        lib_->format_none(mh_);
        static const long rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
        for (long r : rates) lib_->format(mh_, r, mpg::kMono | mpg::kStereo, mpg::kEncSigned16);
        if (lib_->open(mh_, path.c_str()) != mpg::kOk) {
            close();
            return kDecodeFailed;
        }
        long rate = 0;
        int ch = 0, enc = 0;
        if (lib_->getformat(mh_, &rate, &ch, &enc) != mpg::kOk || rate <= 0 || ch < 1 || ch > 2) {
            close();
            return kDecodeFailed;
        }
        info = TrackInfo();
        info.codec = Codec::Mp3;
        info.label = "MP3";
        info.lossy = true;
        info.out.rate = static_cast<uint32_t>(rate);
        info.out.channels = static_cast<uint16_t>(ch);
        info.out.bits = 16;
        const int64_t len = lib_->length(mh_);
        if (len > 0) {
            info.total_frames = static_cast<uint64_t>(len);
            info.frames_known = true;
        }
        fmt_ = info.out;
        pos_ = 0;
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (!mh_) return -EBADF;
        const size_t fb = fmt_.frame_bytes();
        size_t done_bytes = 0;
        for (int tries = 0; tries < 8 && done_bytes == 0; ++tries) {
            size_t done = 0;
            const int r = lib_->read(mh_, out, frames * fb, &done);
            done_bytes = done;
            if (r == mpg::kDone) break;
            if (r == mpg::kNewFormat) {
                long rate = 0;
                int ch = 0, enc = 0;
                lib_->getformat(mh_, &rate, &ch, &enc);
                if (rate != static_cast<long>(fmt_.rate) || ch != fmt_.channels) return kDecodeFailed; /* mid-file change */
                continue;
            }
            if (r == mpg::kNeedMore) continue;
            if (r != mpg::kOk) return done_bytes ? static_cast<long>(done_bytes / fb) : kDecodeFailed;
        }
        const size_t f = done_bytes / fb;
        pos_ += f;
        return static_cast<long>(f);
    }

    int seek(uint64_t frame) override {
        if (!mh_) return -EBADF;
        const int64_t r = lib_->seek(mh_, static_cast<int64_t>(frame), SEEK_SET);
        if (r < 0) return kDecodeFailed;
        pos_ = static_cast<uint64_t>(r);
        return 0;
    }

    uint64_t position() const override { return pos_; }

    void close() override {
        if (mh_ && lib_) {
            lib_->close(mh_);
            lib_->destroy(mh_);
        }
        mh_ = nullptr;
    }

private:
    const Mpg123Lib *lib_;
    mpg::Handle *mh_ = nullptr;
    PcmFormat fmt_;
    uint64_t pos_ = 0;
};

/* --------------------------------------------------------------------------------------- Ogg Vorbis (libvorbisfile) */

class VorbisDecoder final : public TrackDecoder {
public:
    explicit VorbisDecoder(const VorbisFileLib *lib) : lib_(lib) {}
    ~VorbisDecoder() override { close(); }

    int open(const std::string &path, TrackInfo &info) override {
        close();
        if (!lib_ || !lib_->complete()) return kNeedsLibrary;
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) return -errno;
        struct stat st;
        if (::fstat(fd_, &st) == 0) size_ = static_cast<uint64_t>(st.st_size);
        fpos_ = 0;
        vf_.assign(ov::kFileBytes, 0);
        ov::Callbacks cb{&VorbisDecoder::cb_read, &VorbisDecoder::cb_seek, &VorbisDecoder::cb_close,
                         &VorbisDecoder::cb_tell};
        if (lib_->open_callbacks(this, vf_.data(), nullptr, 0, cb) != 0) {
            ::close(fd_);
            fd_ = -1;
            vf_.clear();
            return kDecodeFailed;
        }
        opened_ = true;
        const ov::Info *vi = lib_->info(vf_.data(), -1);
        if (!vi || vi->channels < 1 || vi->channels > 8 || vi->rate <= 0) {
            close();
            return kDecodeFailed;
        }
        info = TrackInfo();
        info.codec = Codec::Vorbis;
        info.label = "Vorbis";
        info.lossy = true;
        info.out.rate = static_cast<uint32_t>(vi->rate);
        info.out.channels = static_cast<uint16_t>(vi->channels);
        info.out.bits = 16;
        const int64_t total = lib_->pcm_total(vf_.data(), -1);
        if (total > 0) {
            info.total_frames = static_cast<uint64_t>(total);
            info.frames_known = true;
        }
        const long br = lib_->bitrate(vf_.data(), -1);
        if (br > 0) info.bitrate_kbps = static_cast<unsigned>((br + 500) / 1000);
        fmt_ = info.out;
        pos_ = 0;
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (!opened_) return -EBADF;
        const size_t fb = fmt_.frame_bytes();
        char *o = static_cast<char *>(out);
        size_t want = frames * fb, got = 0;
        while (got < want) {
            int bs = 0;
            const long r = lib_->read(vf_.data(), o + got, static_cast<int>(std::min<size_t>(want - got, 1 << 16)), 0, 2,
                                      1, &bs);
            if (r == 0) break;
            if (r < 0) {
                if (r == -3 /* OV_HOLE */) continue;
                if (got) break;
                return kDecodeFailed;
            }
            const ov::Info *vi = lib_->info(vf_.data(), -1);
            if (vi && (vi->channels != fmt_.channels || vi->rate != static_cast<long>(fmt_.rate))) {
                /* a chained stream changed its format: stop this track at the change */
                break;
            }
            got += static_cast<size_t>(r);
            if (got % fb == 0 && got >= fb * 256) break; /* enough for one call */
        }
        const size_t f = got / fb;
        pos_ += f;
        return static_cast<long>(f);
    }

    int seek(uint64_t frame) override {
        if (!opened_) return -EBADF;
        if (lib_->pcm_seek(vf_.data(), static_cast<int64_t>(frame)) != 0) return kDecodeFailed;
        pos_ = frame;
        return 0;
    }

    uint64_t position() const override { return pos_; }

    void close() override {
        if (opened_ && lib_) lib_->clear(vf_.data()); /* calls cb_close */
        opened_ = false;
        vf_.clear();
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    static size_t cb_read(void *ptr, size_t size, size_t nmemb, void *src) {
        auto *self = static_cast<VorbisDecoder *>(src);
        const size_t want = size * nmemb;
        if (want == 0 || self->fd_ < 0) return 0;
        const ssize_t k = pread_full(self->fd_, ptr, want, self->fpos_);
        if (k <= 0) return 0;
        self->fpos_ += static_cast<uint64_t>(k);
        return static_cast<size_t>(k) / size;
    }
    static int cb_seek(void *src, int64_t offset, int whence) {
        auto *self = static_cast<VorbisDecoder *>(src);
        int64_t base = 0;
        if (whence == SEEK_CUR) base = static_cast<int64_t>(self->fpos_);
        else if (whence == SEEK_END) base = static_cast<int64_t>(self->size_);
        const int64_t p = base + offset;
        if (p < 0) return -1;
        self->fpos_ = static_cast<uint64_t>(p);
        return 0;
    }
    static int cb_close(void *) { return 0; } /* the fd is closed by close() */
    static long cb_tell(void *src) { return static_cast<long>(static_cast<VorbisDecoder *>(src)->fpos_); }

    const VorbisFileLib *lib_;
    std::vector<unsigned char> vf_;
    bool opened_ = false;
    int fd_ = -1;
    uint64_t size_ = 0, fpos_ = 0;
    PcmFormat fmt_;
    uint64_t pos_ = 0;
};

/* ------------------------------------------------------------------- FLAC and Opus: Cassette's decoders, adapted */

/* Cassette's Decoder delivers 16-bit, packed 24-bit or 32-bit little-endian samples; Vinyl plays int16 or int32. */
class CopiedDecoder final : public TrackDecoder {
public:
    CopiedDecoder(std::unique_ptr<Decoder> d, Codec c) : d_(std::move(d)), codec_(c) {}
    ~CopiedDecoder() override { close(); }

    int open(const std::string &path, TrackInfo &info) override {
        DecodedInfo di;
        const int r = d_->open(posix_file_ops(), path, di);
        if (r != 0) return r;
        in_bits_ = di.output.bits;
        info = TrackInfo();
        info.codec = codec_;
        info.label = codec_name(codec_);
        info.lossy = codec_ == Codec::Opus;
        info.source_bits = codec_ == Codec::Flac ? di.source.bits : 0;
        info.out.rate = di.output.rate;
        info.out.channels = di.output.channels;
        info.out.bits = in_bits_ <= 16 ? 16 : 32;
        info.total_frames = di.total_frames;
        info.frames_known = di.total_frames > 0;
        fmt_ = info.out;
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (in_bits_ == 16 || in_bits_ == 32) return d_->read(out, frames);
        /* packed 24-bit: decode into a scratch buffer, widen to int32 */
        const size_t samples = frames * fmt_.channels;
        tmp_.resize(samples * 3);
        const long n = d_->read(tmp_.data(), frames);
        if (n <= 0) return n;
        int32_t *o = static_cast<int32_t *>(out);
        const unsigned char *p = tmp_.data();
        for (size_t i = 0; i < static_cast<size_t>(n) * fmt_.channels; ++i, p += 3) {
            const uint32_t u = static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                               static_cast<uint32_t>(p[2]) << 24;
            o[i] = static_cast<int32_t>(u);
        }
        return n;
    }

    int seek(uint64_t frame) override { return d_->seek(frame); }
    uint64_t position() const override { return d_->position(); }
    void close() override { d_->close(); }

private:
    std::unique_ptr<Decoder> d_;
    Codec codec_;
    unsigned in_bits_ = 16;
    PcmFormat fmt_;
    std::vector<unsigned char> tmp_;
};

/* ---------------------------------------------------------------------------------------- WAV and AIFF (own reader) */

enum class PcmKind { Int, Float };

class PcmDecoder final : public TrackDecoder {
public:
    ~PcmDecoder() override { close(); }

    int open(const std::string &path, TrackInfo &info) override {
        close();
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) return -errno;
        struct stat st;
        if (::fstat(fd_, &st) != 0) {
            const int e = -errno;
            close();
            return e;
        }
        size_ = static_cast<uint64_t>(st.st_size);
        unsigned char h[12];
        if (pread_full(fd_, h, 12, 0) != 12) {
            close();
            return kNotAudio;
        }
        int r = kNotAudio;
        if (std::memcmp(h, "FORM", 4) == 0) r = parse_aiff(h);
        else r = parse_wav(h);
        if (r != 0) {
            close();
            return r;
        }
        if (channels_ < 1 || channels_ > 8 || rate_ < 1000 || rate_ > 768000 || block_ == 0) {
            close();
            return kNotAudio;
        }
        info = TrackInfo();
        info.codec = aiff_ ? Codec::Aiff : Codec::Wav;
        info.label = aiff_ ? "AIFF" : "WAV";
        info.out.rate = rate_;
        info.out.channels = static_cast<uint16_t>(channels_);
        info.out.bits = (kind_ == PcmKind::Int && bits_ <= 16) ? 16 : 32;
        info.source_bits = bits_;
        total_ = data_len_ / block_;
        info.total_frames = total_;
        info.frames_known = true;
        fmt_ = info.out;
        pos_ = 0;
        return 0;
    }

    long read(void *out, size_t frames) override {
        if (fd_ < 0) return -EBADF;
        if (pos_ >= total_) return 0;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(frames, total_ - pos_));
        raw_.resize(n * block_);
        const ssize_t k = pread_full(fd_, raw_.data(), raw_.size(), data_off_ + pos_ * block_);
        if (k < 0) return k;
        const size_t got = static_cast<size_t>(k) / block_;
        convert(raw_.data(), out, got);
        pos_ += got;
        return static_cast<long>(got);
    }

    int seek(uint64_t frame) override {
        pos_ = std::min(frame, total_);
        return 0;
    }
    uint64_t position() const override { return pos_; }
    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    static uint32_t le32(const unsigned char *p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    static uint16_t le16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    static uint32_t be32(const unsigned char *p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
    static uint16_t be16(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
    static uint64_t le64(const unsigned char *p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

    int parse_wav(const unsigned char *h) {
        const bool rf64 = std::memcmp(h, "RF64", 4) == 0 || std::memcmp(h, "BW64", 4) == 0;
        uint64_t pos = 12, ds64_data = 0;
        bool have_fmt = false;
        while (pos + 8 <= size_) {
            unsigned char c[8];
            if (pread_full(fd_, c, 8, pos) != 8) break;
            uint64_t len = le32(c + 4);
            if (std::memcmp(c, "ds64", 4) == 0 && len >= 16) {
                unsigned char d[16];
                if (pread_full(fd_, d, 16, pos + 8) == 16) ds64_data = le64(d + 8);
            } else if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
                unsigned char f[40] = {0};
                pread_full(fd_, f, std::min<uint64_t>(len, 40), pos + 8);
                unsigned tag = le16(f);
                channels_ = le16(f + 2);
                rate_ = le32(f + 4);
                block_ = le16(f + 12);
                bits_ = le16(f + 14);
                if (tag == 0xFFFE && len >= 40) tag = le16(f + 24); /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
                if (tag == 1) kind_ = PcmKind::Int;
                else if (tag == 3) kind_ = PcmKind::Float;
                else return kNotAudio; /* ADPCM, mu-law...: FFmpeg's (open_track retries) */
                if (kind_ == PcmKind::Int && bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32) return kNotAudio;
                if (kind_ == PcmKind::Float && bits_ != 32 && bits_ != 64) return kNotAudio;
                if (block_ != channels_ * (bits_ / 8)) return kNotAudio;
                have_fmt = true;
            } else if (std::memcmp(c, "data", 4) == 0) {
                if (!have_fmt) return kNotAudio;
                data_off_ = pos + 8;
                if (rf64 && len == 0xFFFFFFFFu) len = ds64_data;
                if (len == 0 || data_off_ + len > size_) len = size_ - data_off_; /* a take still recording, or cut */
                data_len_ = len;
                return 0;
            }
            pos += 8 + len + (len & 1);
        }
        return kNotAudio;
    }

    int parse_aiff(const unsigned char *h) {
        aiff_ = true;
        const bool aifc = std::memcmp(h + 8, "AIFC", 4) == 0;
        uint64_t pos = 12;
        bool have_comm = false;
        while (pos + 8 <= size_) {
            unsigned char c[8];
            if (pread_full(fd_, c, 8, pos) != 8) break;
            const uint64_t len = be32(c + 4);
            if (std::memcmp(c, "COMM", 4) == 0 && len >= 18) {
                unsigned char m[26] = {0};
                pread_full(fd_, m, std::min<uint64_t>(len, 26), pos + 8);
                channels_ = be16(m);
                bits_ = be16(m + 6);
                /* 80-bit extended rate */
                const int exp = ((m[8] & 0x7F) << 8 | m[9]) - 16383 - 63;
                uint64_t mant = 0;
                for (int i = 0; i < 8; ++i) mant = (mant << 8) | m[10 + i];
                rate_ = static_cast<uint32_t>(std::ldexp(static_cast<double>(mant), exp) + 0.5);
                kind_ = PcmKind::Int;
                big_ = true;
                if (aifc && len >= 22) {
                    if (std::memcmp(m + 18, "NONE", 4) == 0 || std::memcmp(m + 18, "twos", 4) == 0) big_ = true;
                    else if (std::memcmp(m + 18, "sowt", 4) == 0) big_ = false;
                    else if (std::memcmp(m + 18, "fl32", 4) == 0 || std::memcmp(m + 18, "FL32", 4) == 0) {
                        kind_ = PcmKind::Float;
                        bits_ = 32;
                    } else {
                        return kNotAudio;
                    }
                }
                if (kind_ == PcmKind::Int && bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32) return kNotAudio;
                block_ = channels_ * (bits_ / 8);
                have_comm = true;
            } else if (std::memcmp(c, "SSND", 4) == 0 && len >= 8) {
                if (!have_comm) return kNotAudio;
                unsigned char s[8];
                if (pread_full(fd_, s, 8, pos + 8) != 8) return kNotAudio;
                data_off_ = pos + 16 + be32(s);
                uint64_t dl = len - 8 - be32(s);
                if (data_off_ + dl > size_) dl = size_ > data_off_ ? size_ - data_off_ : 0;
                data_len_ = dl;
                return 0;
            }
            pos += 8 + len + (len & 1);
        }
        return kNotAudio;
    }

    int32_t sample_at(const unsigned char *p) const {
        if (kind_ == PcmKind::Float) {
            double v;
            if (bits_ == 64) {
                uint64_t u = 0;
                for (int i = 7; i >= 0; --i) u = (u << 8) | p[big_ ? 7 - i : i];
                std::memcpy(&v, &u, 8);
            } else {
                uint32_t u = big_ ? be32(p) : le32(p);
                float f;
                std::memcpy(&f, &u, 4);
                v = f;
            }
            if (!(v == v)) v = 0; /* NaN */
            v = std::clamp(v, -1.0, 1.0);
            return static_cast<int32_t>(std::lround(v * 2147483647.0));
        }
        switch (bits_) {
        case 8: { /* WAV 8-bit is unsigned, AIFF signed; the sample moves to the top byte (no shift of a negative value) */
            const uint32_t u = aiff_ ? static_cast<uint32_t>(p[0]) : static_cast<uint32_t>(p[0] ^ 0x80);
            return static_cast<int32_t>(u << 24);
        }
        case 16: {
            const uint32_t u = big_ ? be16(p) : le16(p);
            return static_cast<int32_t>(u << 16);
        }
        case 24: {
            const uint32_t u = big_ ? (static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8)
                                    : (static_cast<uint32_t>(p[2]) << 24 | p[1] << 16 | p[0] << 8);
            return static_cast<int32_t>(u);
        }
        default: return static_cast<int32_t>(big_ ? be32(p) : le32(p));
        }
    }

    void convert(const unsigned char *in, void *out, size_t frames) const {
        const size_t samples = frames * channels_;
        const unsigned bps = bits_ / 8;
        if (fmt_.bits == 16) {
            int16_t *o = static_cast<int16_t *>(out);
            for (size_t i = 0; i < samples; ++i) o[i] = static_cast<int16_t>(sample_at(in + i * bps) >> 16);
        } else {
            int32_t *o = static_cast<int32_t *>(out);
            for (size_t i = 0; i < samples; ++i) o[i] = sample_at(in + i * bps);
        }
    }

    int fd_ = -1;
    uint64_t size_ = 0, data_off_ = 0, data_len_ = 0, total_ = 0, pos_ = 0;
    unsigned channels_ = 0, bits_ = 0, block_ = 0;
    uint32_t rate_ = 0;
    PcmKind kind_ = PcmKind::Int;
    bool aiff_ = false, big_ = false;
    PcmFormat fmt_;
    std::vector<unsigned char> raw_;
};

}  // namespace

std::unique_ptr<TrackDecoder> make_mp3_decoder(const Mpg123Lib *lib) { return std::make_unique<Mp3Decoder>(lib); }
std::unique_ptr<TrackDecoder> make_vorbis_decoder(const VorbisFileLib *lib) { return std::make_unique<VorbisDecoder>(lib); }
std::unique_ptr<TrackDecoder> make_flac_decoder(const FlacDecLib *lib) {
    return std::make_unique<CopiedDecoder>(std::make_unique<FlacDecoder>(lib), Codec::Flac);
}
std::unique_ptr<TrackDecoder> make_opus_decoder(const OpusDecLib *lib) {
    return std::make_unique<CopiedDecoder>(std::make_unique<OpusFileDecoder>(lib), Codec::Opus);
}
std::unique_ptr<TrackDecoder> make_pcm_decoder() { return std::make_unique<PcmDecoder>(); }

DecodeLibs real_decode_libs() {
    DecodeLibs l;
    l.mpg123 = [] { return real_mpg123(); };
    l.vorbis = [] { return real_vorbisfile(); };
    l.flac = [] { return real_flac_decoder(); };
    l.opus = [] { return real_opus_decoder(); };
    l.ffmpeg = [] { return real_ffmpeg(); };
    return l;
}

std::unique_ptr<TrackDecoder> open_track(const std::string &path, const DecodeLibs &libs, TrackInfo &info, int *err) {
    int e = 0;
    const Codec c = probe_codec(path, &e);
    if (c == Codec::Unknown) {
        if (err) *err = e ? e : kNotAudio;
        return nullptr;
    }
    std::unique_ptr<TrackDecoder> d;
    bool lib_missing = false;
    switch (c) {
    case Codec::Mp3: {
        const Mpg123Lib *l = libs.mpg123 ? libs.mpg123() : nullptr;
        if (l) d = make_mp3_decoder(l);
        else lib_missing = true;
        break;
    }
    case Codec::Vorbis: {
        const VorbisFileLib *l = libs.vorbis ? libs.vorbis() : nullptr;
        if (l) d = make_vorbis_decoder(l);
        else lib_missing = true;
        break;
    }
    case Codec::Flac: {
        const FlacDecLib *l = libs.flac ? libs.flac() : nullptr;
        if (l) d = make_flac_decoder(l);
        else lib_missing = true;
        break;
    }
    case Codec::Opus: {
        const OpusDecLib *l = libs.opus ? libs.opus() : nullptr;
        if (l) d = make_opus_decoder(l);
        else lib_missing = true;
        break;
    }
    case Codec::Wav:
    case Codec::Aiff: d = make_pcm_decoder(); break;
    default: break;
    }
    if (d) {
        const int r = d->open(path, info);
        if (r == 0) {
            if (err) *err = 0;
            return d;
        }
        /* a WAV/AIFF of a compressed kind, an Ogg FLAC, an Opus surround file...: FFmpeg may read it */
        e = r;
        d.reset();
        if (r != kNotAudio && r != kCodecBadStream && r != kDecodeFailed) {
            if (err) *err = r;
            return nullptr;
        }
    } else if (lib_missing) {
        if (err) *err = kNeedsLibrary;
        return nullptr;
    }
    const FfmpegLib *ff = libs.ffmpeg ? libs.ffmpeg() : nullptr;
    if (!ff) {
        if (err) *err = (c == Codec::Ffmpeg) ? kNeedsFfmpeg : (e ? e : kNotAudio);
        return nullptr;
    }
    d = make_ffmpeg_decoder(ff);
    const int r = d->open(path, info);
    if (r != 0) {
        if (err) *err = r;
        return nullptr;
    }
    if (err) *err = 0;
    return d;
}

std::string track_error_text(int err, const std::string &path) {
    const std::string e = lower_ext(path);
    switch (err) {
    case kNeedsFfmpeg: return "The FFmpeg libraries could not be loaded. Reinstall Vinyl.";
    case kNeedsLibrary:
        if (e == "mp3" || e == "mp2") return std::string("MP3 needs ") + kMpg123Package + ". Install it from Settings, Apps.";
        if (e == "ogg" || e == "oga") return std::string("Vorbis needs ") + kVorbisPackage + ". Install it from Settings, Apps.";
        if (e == "flac") return "FLAC needs libflac14. Install it from Settings, Apps.";
        if (e == "opus") return "Opus needs libopus0. Install it from Settings, Apps.";
        return "A library for this format is missing. Install Vinyl again from Settings, Apps.";
    case kNotAudio: return "Not a music file Vinyl can play.";
    case kDecodeFailed:
    case kCodecBadStream: return "This file is damaged or in a variant Vinyl cannot read.";
    case kCodecMissing: return "A library for this format is missing. Install Vinyl again from Settings, Apps.";
    default: break;
    }
    if (err == -ENOENT) return "The file is gone (moved, renamed or on a removed drive).";
    if (err == -EACCES || err == -EPERM) return "The file cannot be read (permission).";
    if (err < 0 && err > -4096) return std::string("The file cannot be read: ") + std::strerror(-err) + ".";
    return "The file cannot be played.";
}

}  // namespace vinyl
