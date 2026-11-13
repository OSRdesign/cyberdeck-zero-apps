/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the decoders on the build box's REAL libraries (libmpg123, libvorbisfile, libFLAC, libopus, FFmpeg) and
 * files that tests/run.sh makes with the command-line encoders (lame, flac, oggenc, opusenc, ffmpeg) into
 * $VINYL_FIXTURES. Without that folder, or without a library, the group is skipped. Checked per format: the format
 * found, the length against the encoder's, a seek against a full decode (sample exact where the library is; FFmpeg to
 * the packet), the tags, and a gapless join of two FLAC halves of one sine through the engine.
 */
#include "check.hpp"

#include "decode/track_decoder.hpp"
#include "engine/engine.hpp"
#include "tags/tag_reader.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace vinyl;

namespace {

std::vector<int32_t> decode_all(TrackDecoder &d, const TrackInfo &info, size_t max_frames) {
    std::vector<int32_t> out;
    std::vector<unsigned char> buf(4096 * info.out.frame_bytes());
    while (out.size() / info.out.channels < max_frames) {
        const long n = d.read(buf.data(), 4096);
        if (n <= 0) break;
        for (long i = 0; i < n * info.out.channels; ++i)
            out.push_back(info.out.bits == 16 ? reinterpret_cast<int16_t *>(buf.data())[i] : reinterpret_cast<int32_t *>(buf.data())[i]);
    }
    return out;
}

struct Rec final : PlaybackSink {
    std::mutex *mu;
    std::vector<int32_t> *samples;
    int *opens;
    SinkFormat f;
    int open(const AudioFormat &s, SinkFormat &g) override {
        g.rate = s.rate;
        g.channels = s.channels;
        g.format = s.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        f = g;
        ++*opens;
        return 0;
    }
    long write(const void *p, size_t n) override {
        std::lock_guard<std::mutex> l(*mu);
        for (size_t i = 0; i < n * f.channels; ++i)
            samples->push_back(f.format == SampleFormat::S16_LE ? static_cast<const int16_t *>(p)[i] : static_cast<const int32_t *>(p)[i]);
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override { return 0; }
    void drop() override {}
    void drain() override {}
    void close() override {}
};

struct RecProvider final : SinkProvider {
    std::mutex mu;
    std::vector<int32_t> samples;
    int opens = 0;
    int open(const OutputTarget &, OpenedSink &o, std::string &) override {
        auto s = std::make_unique<Rec>();
        s->mu = &mu;
        s->samples = &samples;
        s->opens = &opens;
        o.sink = std::move(s);
        o.label = "rec";
        o.name = "rec";
        return 0;
    }
    bool still_there(const OpenedSink &) override { return false; }
};

}  // namespace

void test_real_libs() {
    const char *fx = std::getenv("VINYL_FIXTURES");
    if (!fx || !*fx) {
        SKIP("real libraries: VINYL_FIXTURES not set (tests/run.sh makes the files)");
        return;
    }
    const std::string d = fx;
    const DecodeLibs libs = real_decode_libs();
    struct Case {
        const char *file;
        Codec codec;
        const char *label;
        uint32_t rate;
        unsigned bits;  /* 16 or 32 out */
        double seconds; /* the encoder's length */
        double tol_s;   /* length tolerance */
        bool exact_seek;
        const char *lib; /* the library the case needs */
    };
    const Case cases[] = {
        {"sine.mp3", Codec::Mp3, "MP3", 44100, 16, 6.0, 0.05, true, "mpg123"},
        {"sine.flac", Codec::Flac, "FLAC", 44100, 16, 6.0, 0.001, true, "flac"},
        {"sine24.flac", Codec::Flac, "FLAC", 96000, 32, 6.0, 0.001, true, "flac"},
        {"sine.ogg", Codec::Vorbis, "Vorbis", 44100, 16, 6.0, 0.01, true, "vorbis"},
        {"sine.opus", Codec::Opus, "Opus", 48000, 16, 6.0, 0.03, true, "opus"},
        {"sine.wav", Codec::Wav, "WAV", 44100, 16, 6.0, 0.001, true, ""},
        {"sine.m4a", Codec::Ffmpeg, "AAC", 44100, 16, 6.0, 0.1, false, "ffmpeg"},
        {"sine-alac.m4a", Codec::Ffmpeg, "ALAC", 44100, 16, 6.0, 0.01, false, "ffmpeg"},
    };
    for (const Case &c : cases) {
        const std::string lib = c.lib;
        if ((lib == "mpg123" && !real_mpg123()) || (lib == "vorbis" && !real_vorbisfile()) || (lib == "flac" && !real_flac_decoder()) ||
            (lib == "opus" && !real_opus_decoder()) || (lib == "ffmpeg" && !real_ffmpeg())) {
            SKIP(c.file);
            continue;
        }
        const std::string path = d + "/" + c.file;
        TrackInfo info;
        int err = 0;
        auto dec = open_track(path, libs, info, &err);
        if (!dec) {
            std::fprintf(stderr, "%s: open failed %d (%s)\n", c.file, err, track_error_text(err, path).c_str());
            CHECK(false);
            continue;
        }
        CHECK(info.codec == c.codec);
        CHECK_EQ(info.label, std::string(c.label));
        CHECK_EQ(info.out.rate, c.rate);
        CHECK_EQ(static_cast<unsigned>(info.out.bits), c.bits);
        const std::vector<int32_t> all = decode_all(*dec, info, 1u << 24);
        const double secs = static_cast<double>(all.size() / info.out.channels) / info.out.rate;
        std::fprintf(stderr, "  %-14s %s %u Hz %u ch out %u bit, %.4f s decoded, header %.4f s\n", c.file, info.label.c_str(),
                     info.out.rate, info.out.channels, info.out.bits, secs, info.total_frames / static_cast<double>(info.out.rate));
        CHECK_NEAR(secs, c.seconds, c.tol_s);
        /* seek to 4.5 s, compare with the full decode */
        const uint64_t at = static_cast<uint64_t>(4.5 * info.out.rate);
        CHECK_EQ(dec->seek(at), 0);
        const std::vector<int32_t> part = decode_all(*dec, info, 256);
        if (c.exact_seek && part.size() >= 256 && all.size() >= (at + 256) * info.out.channels) {
            bool same = true;
            for (size_t i = 0; i < 256 && same; ++i) same = part[i] == all[at * info.out.channels + i];
            CHECK(same);
        } else if (!c.exact_seek) {
            CHECK(!part.empty()); /* FFmpeg: lands on the packet at or before the frame */
        }
        TrackTags t;
        CHECK(read_tags(path, t));
        CHECK_EQ(t.title, std::string("Vinyl test sine"));
        CHECK_NEAR(t.duration_ms / 1000.0, c.seconds, c.tol_s + 0.06);
    }
    /* the CJK file of the study's test set, when present */
    if (const char *cjk = std::getenv("VINYL_CJK_MP3"); cjk && *cjk) {
        TrackTags t;
        Picture pic;
        CHECK(read_tags(cjk, t, &pic));
        CHECK_EQ(t.title, std::string("夜明けのスキャット（テスト曲）"));
        CHECK_EQ(t.artist, std::string("测试艺术家"));
        CHECK_EQ(t.album, std::string("테스트 앨범 059"));
        CHECK(!pic.data.empty());
    }
    /* gapless through the engine: two FLAC halves of one sine come out as the whole */
    if (real_flac_decoder()) {
        RecProvider p;
        Engine e(libs, p);
        e.load(0, d + "/half1.flac", 0, true);
        e.set_next(1, d + "/half2.flac");
        bool end = false;
        for (int i = 0; i < 400 && !end; ++i) {
            EngineEvent ev;
            while (e.poll_event(ev))
                if (ev.kind == EngineEvent::Kind::QueueEnd) end = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(end);
        TrackInfo info;
        auto whole = open_track(d + "/whole.flac", libs, info, nullptr);
        CHECK(whole != nullptr);
        if (whole) {
            const std::vector<int32_t> ref = decode_all(*whole, info, 1u << 24);
            std::lock_guard<std::mutex> l(p.mu);
            CHECK_EQ(p.opens, 1);
            CHECK_EQ(p.samples.size(), ref.size());
            CHECK(p.samples == ref);
        }
    } else {
        SKIP("gapless FLAC: no libFLAC");
    }
}
