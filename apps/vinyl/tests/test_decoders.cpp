/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the probe, the WAV / AIFF reader (16, 24, 8 bit, float, big endian, sowt), the MP3 decoder on a fake
 * libmpg123 table, the factory's errors (a missing library, FFmpeg absent), the error texts, and the PCM conversion of
 * the engine.
 */
#include "check.hpp"

#include "decode/track_decoder.hpp"
#include "engine/engine.hpp"

#include <cstring>

using namespace vinyl;

namespace {

std::string wav(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, const std::string &data) {
    std::string f = "fmt ";
    put_le32(f, 16);
    put_le16(f, tag);
    put_le16(f, channels);
    put_le32(f, rate);
    put_le32(f, rate * channels * (bits / 8));
    put_le16(f, static_cast<uint16_t>(channels * (bits / 8)));
    put_le16(f, bits);
    std::string d = "data";
    put_le32(d, static_cast<uint32_t>(data.size()));
    d += data;
    std::string w = "RIFF";
    put_le32(w, static_cast<uint32_t>(4 + f.size() + d.size()));
    return w + "WAVE" + f + d;
}

/* ---- a fake libmpg123: 1000 frames of stereo, sample = frame index (left) and -index (right) ---- */
struct FakeMpg {
    long pos = 0;
    long total = 0;
    bool open = false;
    bool new_format_once = true;
};
FakeMpg g_mpg;

mpg::Handle *f_new(const char *, int *e) {
    *e = 0;
    g_mpg = FakeMpg();
    return reinterpret_cast<mpg::Handle *>(&g_mpg);
}
void f_delete(mpg::Handle *) {}
int f_param(mpg::Handle *, long, long, double) { return 0; }
int f_format_none(mpg::Handle *) { return 0; }
int f_format(mpg::Handle *, long, int, int) { return 0; }
int f_open(mpg::Handle *, const char *) {
    g_mpg.open = true;
    g_mpg.total = 1000;
    return 0;
}
int f_close(mpg::Handle *) {
    g_mpg.open = false;
    return 0;
}
int f_getformat(mpg::Handle *, long *r, int *c, int *e) {
    *r = 44100;
    *c = 2;
    *e = mpg::kEncSigned16;
    return 0;
}
int f_read(mpg::Handle *, void *out, size_t size, size_t *done) {
    if (g_mpg.new_format_once) {
        g_mpg.new_format_once = false;
        *done = 0;
        return mpg::kNewFormat;
    }
    int16_t *o = static_cast<int16_t *>(out);
    size_t frames = size / 4, n = 0;
    while (n < frames && g_mpg.pos < g_mpg.total) {
        o[2 * n] = static_cast<int16_t>(g_mpg.pos);
        o[2 * n + 1] = static_cast<int16_t>(-g_mpg.pos);
        ++n;
        ++g_mpg.pos;
    }
    *done = n * 4;
    return n ? mpg::kOk : mpg::kDone;
}
int64_t f_seek(mpg::Handle *, int64_t s, int) {
    g_mpg.pos = static_cast<long>(s);
    return s;
}
int64_t f_tell(mpg::Handle *) { return g_mpg.pos; }
int64_t f_length(mpg::Handle *) { return g_mpg.total; }
int f_scan(mpg::Handle *) { return 0; }
const char *f_strerror(mpg::Handle *) { return "fake"; }

Mpg123Lib fake_mpg123() {
    Mpg123Lib l;
    l.name = "fake";
    l.create = f_new;
    l.destroy = f_delete;
    l.param = f_param;
    l.format_none = f_format_none;
    l.format = f_format;
    l.open = f_open;
    l.close = f_close;
    l.getformat = f_getformat;
    l.read = f_read;
    l.seek = f_seek;
    l.tell = f_tell;
    l.length = f_length;
    l.scan = f_scan;
    l.strerror = f_strerror;
    return l;
}

}  // namespace

void test_decoders() {
    const std::string dir = make_temp_dir("dec");
    /* the probe */
    {
        write_file(dir + "/a.wav", wav(1, 1, 8000, 16, std::string(16, '\0')));
        CHECK(probe_codec(dir + "/a.wav") == Codec::Wav);
        write_file(dir + "/b.flac", "fLaC" + std::string(60, '\0'));
        CHECK(probe_codec(dir + "/b.flac") == Codec::Flac);
        std::string mp3 = std::string("\xFF\xFB\x90\x64", 4) + std::string(60, '\0');
        write_file(dir + "/c.mp3", mp3);
        CHECK(probe_codec(dir + "/c.mp3") == Codec::Mp3);
        std::string id3 = std::string("ID3\x03\x00\x00\x00\x00\x00\x0A", 10) + std::string(10, '\0') + mp3;
        write_file(dir + "/d.mp3", id3);
        CHECK(probe_codec(dir + "/d.mp3") == Codec::Mp3);
        std::string id3flac = std::string("ID3\x03\x00\x00\x00\x00\x00\x04", 10) + std::string(4, '\0') + "fLaC" + std::string(40, '\0');
        write_file(dir + "/e.flac", id3flac);
        CHECK(probe_codec(dir + "/e.flac") == Codec::Flac);
        std::string adts = std::string("\xFF\xF1\x50\x80", 4) + std::string(60, '\0'); /* AAC ADTS: layer 0, not MP3 */
        write_file(dir + "/f.aac", adts);
        CHECK(probe_codec(dir + "/f.aac") == Codec::Ffmpeg);
        std::string m4a(8, '\0');
        m4a[3] = 0x20;
        m4a.replace(4, 4, "ftyp");
        write_file(dir + "/g.m4a", m4a + std::string(40, '\0'));
        CHECK(probe_codec(dir + "/g.m4a") == Codec::Ffmpeg);
        std::string ogg = "OggS";
        ogg += std::string(22, '\0');
        ogg += '\x01';
        ogg += '\x13';
        std::string opus = ogg + "OpusHead" + std::string(20, '\0');
        write_file(dir + "/h.opus", opus);
        CHECK(probe_codec(dir + "/h.opus") == Codec::Opus);
        std::string vorbis = ogg + "\x01vorbis" + std::string(30, '\0');
        write_file(dir + "/i.ogg", vorbis);
        CHECK(probe_codec(dir + "/i.ogg") == Codec::Vorbis);
        int err = 0;
        CHECK(probe_codec(dir + "/none.mp3", &err) == Codec::Unknown);
        CHECK(err < 0);
        CHECK(is_audio_name("x.FLAC") && is_audio_name("y.m4a") && !is_audio_name("cover.jpg") && !is_audio_name("noext"));
        CHECK(is_playlist_name("a.M3U8") && !is_playlist_name("a.pls"));
    }
    /* WAV 16-bit stereo: values and seek */
    {
        std::string d;
        for (int i = 0; i < 100; ++i) {
            put_le16(d, static_cast<uint32_t>(i * 100));
            put_le16(d, static_cast<uint32_t>(-(i * 100)) & 0xFFFF);
        }
        write_file(dir + "/s16.wav", wav(1, 2, 44100, 16, d));
        DecodeLibs none;
        TrackInfo info;
        int err = 0;
        auto dec = open_track(dir + "/s16.wav", none, info, &err);
        CHECK(dec != nullptr);
        if (dec) {
            CHECK(info.codec == Codec::Wav);
            CHECK_EQ(info.out.rate, 44100u);
            CHECK_EQ(info.out.bits, static_cast<uint16_t>(16));
            CHECK_EQ(info.total_frames, static_cast<uint64_t>(100));
            CHECK_EQ(info.source_bits, 16u);
            int16_t buf[40];
            CHECK_EQ(dec->read(buf, 10), 10L);
            CHECK_EQ(buf[2], static_cast<int16_t>(100));
            CHECK_EQ(buf[3], static_cast<int16_t>(-100));
            CHECK_EQ(dec->seek(50), 0);
            CHECK_EQ(dec->read(buf, 1), 1L);
            CHECK_EQ(buf[0], static_cast<int16_t>(5000));
            CHECK_EQ(dec->position(), static_cast<uint64_t>(51));
            dec->seek(98);
            CHECK_EQ(dec->read(buf, 20), 2L);
            CHECK_EQ(dec->read(buf, 20), 0L);
        }
    }
    /* WAV 24-bit mono -> int32 with the sample at the top; 8-bit unsigned; float */
    {
        std::string d;
        for (int v : {0x123456, -0x123456, 0x7FFFFF}) {
            const uint32_t u = static_cast<uint32_t>(v) & 0xFFFFFF;
            d += static_cast<char>(u & 0xFF);
            d += static_cast<char>((u >> 8) & 0xFF);
            d += static_cast<char>((u >> 16) & 0xFF);
        }
        write_file(dir + "/s24.wav", wav(1, 1, 96000, 24, d));
        TrackInfo info;
        auto dec = open_track(dir + "/s24.wav", DecodeLibs(), info, nullptr);
        CHECK(dec != nullptr);
        if (dec) {
            CHECK_EQ(info.out.bits, static_cast<uint16_t>(32));
            CHECK_EQ(info.source_bits, 24u);
            int32_t b[3];
            CHECK_EQ(dec->read(b, 3), 3L);
            CHECK_EQ(b[0], static_cast<int32_t>(0x12345600));
            CHECK_EQ(b[1], static_cast<int32_t>(static_cast<uint32_t>(-0x123456) << 8));
            CHECK_EQ(b[2], static_cast<int32_t>(0x7FFFFF00));
        }
        write_file(dir + "/u8.wav", wav(1, 1, 8000, 8, std::string("\x80\xFF\x00", 3)));
        dec = open_track(dir + "/u8.wav", DecodeLibs(), info, nullptr);
        CHECK(dec != nullptr);
        if (dec) {
            int16_t b[3];
            CHECK_EQ(dec->read(b, 3), 3L);
            CHECK_EQ(b[0], static_cast<int16_t>(0));
            CHECK_EQ(b[1], static_cast<int16_t>(127 << 8));
            CHECK_EQ(b[2], static_cast<int16_t>(-32768));
        }
        std::string fd;
        for (float f : {0.5f, -1.0f, 2.0f}) {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            put_le32(fd, u);
        }
        write_file(dir + "/f32.wav", wav(3, 1, 48000, 32, fd));
        dec = open_track(dir + "/f32.wav", DecodeLibs(), info, nullptr);
        CHECK(dec != nullptr);
        if (dec) {
            CHECK_EQ(info.out.bits, static_cast<uint16_t>(32));
            int32_t b[3];
            CHECK_EQ(dec->read(b, 3), 3L);
            CHECK_NEAR(b[0], 1073741824.0, 2);
            CHECK_NEAR(b[1], -2147483647.0, 2);
            CHECK_NEAR(b[2], 2147483647.0, 2); /* clipped */
        }
        /* ADPCM (tag 2): not ours, and without FFmpeg a clear error */
        write_file(dir + "/adpcm.wav", wav(2, 1, 8000, 16, std::string(8, '\0')));
        int err = 0;
        dec = open_track(dir + "/adpcm.wav", DecodeLibs(), info, &err);
        CHECK(dec == nullptr);
        CHECK(err == kNotAudio || err == kNeedsFfmpeg);
    }
    /* AIFF big endian and AIFC sowt */
    {
        auto aiff = [&](bool aifc, const char *comp, const std::string &data) {
            std::string comm;
            put_be16(comm, 1);
            put_be32(comm, static_cast<uint32_t>(data.size() / 2));
            put_be16(comm, 16);
            comm += std::string("\x40\x0E\xAC\x44\x00\x00\x00\x00\x00\x00", 10);
            if (aifc) comm += std::string(comp) + std::string(2, '\0');
            std::string c = "COMM";
            put_be32(c, static_cast<uint32_t>(comm.size()));
            c += comm;
            std::string s = "SSND";
            put_be32(s, static_cast<uint32_t>(8 + data.size()));
            put_be32(s, 0);
            put_be32(s, 0);
            s += data;
            std::string body = std::string(aifc ? "AIFC" : "AIFF") + c + s;
            std::string f = "FORM";
            put_be32(f, static_cast<uint32_t>(body.size()));
            return f + body;
        };
        write_file(dir + "/a.aif", aiff(false, "", std::string("\x12\x34\xFF\xFE", 4)));
        TrackInfo info;
        auto dec = open_track(dir + "/a.aif", DecodeLibs(), info, nullptr);
        CHECK(dec != nullptr);
        if (dec) {
            CHECK(info.codec == Codec::Aiff);
            CHECK_EQ(info.out.rate, 44100u);
            int16_t b[2];
            CHECK_EQ(dec->read(b, 2), 2L);
            CHECK_EQ(b[0], static_cast<int16_t>(0x1234));
            CHECK_EQ(b[1], static_cast<int16_t>(-2));
        }
        write_file(dir + "/b.aifc", aiff(true, "sowt", std::string("\x34\x12", 2)));
        dec = open_track(dir + "/b.aifc", DecodeLibs(), info, nullptr);
        CHECK(dec != nullptr);
        if (dec) {
            int16_t b[1];
            CHECK_EQ(dec->read(b, 1), 1L);
            CHECK_EQ(b[0], static_cast<int16_t>(0x1234));
        }
    }
    /* MP3 through a fake libmpg123: the new-format first read, the length, the seek */
    {
        static const Mpg123Lib fake = fake_mpg123();
        DecodeLibs libs;
        libs.mpg123 = [] { return &fake; };
        write_file(dir + "/x.mp3", std::string("\xFF\xFB\x90\x64", 4) + std::string(600, '\0'));
        TrackInfo info;
        int err = 0;
        auto dec = open_track(dir + "/x.mp3", libs, info, &err);
        CHECK(dec != nullptr);
        if (dec) {
            CHECK(info.codec == Codec::Mp3);
            CHECK(info.lossy);
            CHECK_EQ(info.total_frames, static_cast<uint64_t>(1000));
            int16_t b[64];
            CHECK_EQ(dec->read(b, 16), 16L);
            CHECK_EQ(b[2], static_cast<int16_t>(1));
            CHECK_EQ(b[3], static_cast<int16_t>(-1));
            CHECK_EQ(dec->seek(900), 0);
            CHECK_EQ(dec->read(b, 32), 32L);
            CHECK_EQ(b[0], static_cast<int16_t>(900));
            dec->seek(990);
            CHECK_EQ(dec->read(b, 32), 10L);
            CHECK_EQ(dec->read(b, 32), 0L);
        }
        /* the library missing: a clear error naming the package */
        DecodeLibs nolib;
        nolib.mpg123 = [] { return static_cast<const Mpg123Lib *>(nullptr); };
        CHECK(open_track(dir + "/x.mp3", nolib, info, &err) == nullptr);
        CHECK_EQ(err, kNeedsLibrary);
        CHECK(track_error_text(err, "a.mp3").find("libmpg123-0t64") != std::string::npos);
    }
    /* a tier-2 file without FFmpeg: the notice's sentence */
    {
        std::string m4a(8, '\0');
        m4a[3] = 0x20;
        m4a.replace(4, 4, "ftyp");
        write_file(dir + "/y.m4a", m4a + std::string(40, '\0'));
        DecodeLibs libs;
        libs.ffmpeg = [] { return static_cast<const FfmpegLib *>(nullptr); };
        TrackInfo info;
        int err = 0;
        CHECK(open_track(dir + "/y.m4a", libs, info, &err) == nullptr);
        CHECK_EQ(err, kNeedsFfmpeg);
        CHECK_EQ(track_error_text(err, dir + "/y.m4a"),
                 std::string("This format needs the FFmpeg libraries. Install them from Settings, Apps."));
        CHECK(track_error_text(-ENOENT, "x").find("gone") != std::string::npos);
        CHECK_EQ(ffmpeg_codec_label("aac"), std::string("AAC"));
        CHECK_EQ(ffmpeg_codec_label("alac"), std::string("ALAC"));
        CHECK_EQ(ffmpeg_codec_label("wmav2"), std::string("WMA"));
        CHECK_EQ(ffmpeg_codec_label("ape"), std::string("APE"));
        CHECK_EQ(ffmpeg_codec_label("xyz"), std::string("XYZ"));
        CHECK_EQ(av::offsets_for(60).stream_codecpar, static_cast<size_t>(16));
        CHECK_EQ(av::offsets_for(61).stream_codecpar, static_cast<size_t>(16));
    }
    /* the engine's conversion */
    {
        const int16_t mono[3] = {1000, -2000, 32767};
        int16_t st[6];
        convert_pcm(mono, 16, 1, st, SampleFormat::S16_LE, 2, 3, 1.0f);
        CHECK_EQ(st[0], static_cast<int16_t>(1000));
        CHECK_EQ(st[1], static_cast<int16_t>(1000));
        CHECK_EQ(st[5], static_cast<int16_t>(32767));
        const int16_t stereo[4] = {1000, 3000, -100, -300};
        int16_t m[2];
        convert_pcm(stereo, 16, 2, m, SampleFormat::S16_LE, 1, 2, 1.0f);
        CHECK_EQ(m[0], static_cast<int16_t>(2000));
        CHECK_EQ(m[1], static_cast<int16_t>(-200));
        const int32_t six[6] = {0x10000000, 0x20000000, 7, 7, 7, 7};
        int32_t two[2];
        convert_pcm(six, 32, 6, two, SampleFormat::S32_LE, 2, 1, 1.0f);
        CHECK_EQ(two[0], 0x10000000);
        CHECK_EQ(two[1], 0x20000000);
        int16_t s16[2];
        convert_pcm(six, 32, 6, s16, SampleFormat::S16_LE, 2, 1, 1.0f);
        CHECK_EQ(s16[0], static_cast<int16_t>(0x1000));
        int16_t half[2];
        convert_pcm(stereo, 16, 2, half, SampleFormat::S16_LE, 2, 1, 0.5f);
        CHECK_EQ(half[0], static_cast<int16_t>(500));
        int32_t w32[2];
        convert_pcm(stereo, 16, 2, w32, SampleFormat::S32_LE, 2, 1, 1.0f);
        CHECK_EQ(w32[0], 1000 << 16);
        unsigned char p24[6];
        convert_pcm(stereo, 16, 2, p24, SampleFormat::S24_3LE, 2, 1, 1.0f);
        CHECK_EQ(static_cast<int>(p24[1] | (p24[2] << 8)), 1000);
        CHECK_NEAR(volume_gain(50), 0.125, 1e-6); /* cubic: -18.06 dB, the server's curve */
        CHECK_NEAR(volume_gain(40), 0.064, 1e-6); /* -23.88 dB */
        CHECK_NEAR(volume_gain(0), 0.0, 1e-9);
        CHECK_NEAR(volume_gain(100), 1.0, 1e-9);
    }
    rm_tree(dir);
}
