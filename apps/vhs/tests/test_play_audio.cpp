/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests, group "playaudio": the audio side of playback (brief 3.1, 10, 15.1 "Playback"). Parts are Vinyl 0.3.0's tests
 * (apps/vinyl at fdad829: test_engine.cpp, test_keys_outputs.cpp, test_volume.cpp), ported to the copied code:
 *
 *   engine     the engine on WAV files through the REAL FFmpeg decoder and a fake output: gapless joins, a format change, pause
 *              and play, seek, the loss of the output, a profile switch, a next track that cannot be opened, the output
 *              chosen while playing; the heard position is frames written minus the output's delay (a gated sink)
 *   decoder    the FFmpeg decoder on an MKV (MJPEG + AAC made by ffmpeg, and one made by VHS's own Recorder): format, length
 *              (the container's, an MKV audio stream has none), the seek through the video's index, a video-only file
 *              (kNoAudioStream), a missing file, not a media file
 *   player     the AudioPlayer facade over a fake sink: open, play, pause, seek, the heard clock, the end, a video-only file,
 *              open failures, an output that cannot open or goes away, the volume (the sound card's mixer, a software gain, a
 *              sink of the sound server), the media keys, the output chooser, the real world without any sound device
 *   keys, outputs, launcher, volume model, the ALSA mixer fix (audio/alsa merged from Vinyl)
 *
 * The synthetic files come from the build box's ffmpeg (testsrc2, sine and a 1 kHz burst: never a recording of anyone); without
 * ffmpeg, or without FFmpeg's libraries, those cases skip loudly.
 */
#include "check.hpp"

#include "audio/alsa/alsa_api.hpp"
#include "launcher/launcher_link.hpp"
#include "play/audio_player.hpp"
#include "play/audio_player_deps.hpp"
#include "play/decode/track_decoder.hpp"
#include "play/engine/engine.hpp"
#include "play/engine/outputs.hpp"
#include "play/fake_output.hpp"
#include "play/input/media_keys.hpp"
#include "play/volume/output_volume.hpp"
#include "play/volume/volume_state.hpp"
#include "record/av_libs.hpp"
#include "record/recorder.hpp"
#include "scenes/fileops.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace vhs;

namespace {

/* ------------------------------------------------------------------------------------------------ files */

void put_le16(std::string &s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_le32(std::string &s, uint32_t v) {
    put_le16(s, v & 0xFFFF);
    put_le16(s, v >> 16);
}

/* a 16-bit stereo WAV whose left sample is base + frame and right sample -(base + frame) */
std::string ramp_wav(uint32_t rate, int frames, int base) {
    std::string d;
    for (int i = 0; i < frames; ++i) {
        put_le16(d, static_cast<uint32_t>(base + i) & 0xFFFF);
        put_le16(d, static_cast<uint32_t>(-(base + i)) & 0xFFFF);
    }
    std::string f = "fmt ";
    put_le32(f, 16);
    put_le16(f, 1);
    put_le16(f, 2);
    put_le32(f, rate);
    put_le32(f, rate * 4);
    put_le16(f, 4);
    put_le16(f, 16);
    std::string dd = "data";
    put_le32(dd, static_cast<uint32_t>(d.size()));
    std::string w = "RIFF";
    put_le32(w, static_cast<uint32_t>(4 + f.size() + 8 + d.size()));
    return w + "WAVE" + f + dd + d;
}

/* a mono WAV: silence with a 1 kHz burst from burst_s to burst_s + 0.1 s */
std::string burst_wav(uint32_t rate, int seconds, double burst_s) {
    std::string d;
    for (uint32_t i = 0; i < rate * static_cast<uint32_t>(seconds); ++i) {
        const double t = static_cast<double>(i) / rate;
        const int v = t >= burst_s && t < burst_s + 0.1 ? static_cast<int>(std::lround(14000.0 * std::sin(2 * M_PI * 1000.0 * t))) : 0;
        put_le16(d, static_cast<uint32_t>(v) & 0xFFFF);
    }
    std::string f = "fmt ";
    put_le32(f, 16);
    put_le16(f, 1);
    put_le16(f, 1);
    put_le32(f, rate);
    put_le32(f, rate * 2);
    put_le16(f, 2);
    put_le16(f, 16);
    std::string dd = "data";
    put_le32(dd, static_cast<uint32_t>(d.size()));
    std::string w = "RIFF";
    put_le32(w, static_cast<uint32_t>(4 + f.size() + 8 + d.size()));
    return w + "WAVE" + f + dd + d;
}

bool have_tool(const char *t) { return std::system((std::string("command -v ") + t + " > /dev/null 2>&1").c_str()) == 0; }

bool run_ffmpeg(const std::string &args) {
    return std::system(("ffmpeg -hide_banner -loglevel error -y " + args + " > /dev/null 2>&1").c_str()) == 0;
}

/* a scene as VHS records it: MJPEG video and 16 kHz stereo AAC in MKV, made by the Recorder itself (tone: 440 Hz) */
bool make_scene(const std::string &dir, const std::string &stem, int frames, const std::vector<std::string> &jpegs) {
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (!av) return false;
    RecorderConfig rc;
    rc.av = av;
    rc.ops = &posix_file_ops();
    rc.folder = dir;
    rc.stem = stem;
    rc.video.codec = VideoCodec::Mjpeg;
    rc.video.w = 160;
    rc.video.h = 90;
    rc.video.fps = 30;
    const int rate = 16000, ch = 2, fps = 30;
    rc.audio_rate = rate;
    rc.audio_channels = ch;
    rc.ring_seconds = 60;
    rc.free_space = [] { return UINT64_MAX / 2; };
    rc.started_unix = 1763145120;
    Recorder r(rc);
    r.start();
    const std::int64_t ta0 = 2000LL * 1000000000LL; /* the microphone started 0.5 s before the first frame */
    const std::int64_t tv0_us = (ta0 + 500000000LL) / 1000 + 137;
    auto t_of = [&](std::uint64_t p) { return ta0 + static_cast<std::int64_t>(std::llround(static_cast<double>(p) / rate * 1e9)); };
    auto ts_of = [&](int i) { return tv0_us + static_cast<std::int64_t>(i) * 1000000 / fps; };
    std::uint64_t pushed = 0, next_clock = 1024;
    std::vector<std::int16_t> chunk(1024 * static_cast<std::size_t>(ch));
    auto push_audio_until = [&](std::int64_t ns) {
        while (t_of(pushed + 1024) <= ns) {
            for (int i = 0; i < 1024; ++i) {
                const double ph = 2 * M_PI * 440.0 * static_cast<double>(pushed + static_cast<std::uint64_t>(i)) / rate;
                const std::int16_t v = static_cast<std::int16_t>(std::lround(6000.0 * std::sin(ph)));
                for (int c = 0; c < ch; ++c) chunk[static_cast<std::size_t>(i * ch + c)] = v;
            }
            r.write(chunk.data(), 1024);
            pushed += 1024;
            if (pushed >= next_clock) {
                r.audio_clock(t_of(pushed), pushed);
                next_clock += static_cast<std::uint64_t>(rate) * 2;
            }
        }
    };
    for (int i = 0; i < frames; ++i) {
        push_audio_until(ts_of(i) * 1000 + 40000000LL);
        const std::string &j = jpegs[static_cast<std::size_t>(i) % jpegs.size()];
        r.push_video(reinterpret_cast<const std::uint8_t *>(j.data()), j.size(), ts_of(i));
    }
    push_audio_until(ts_of(frames) * 1000 + 100000000LL);
    wait_for(5000, [&] { return r.status().frames >= static_cast<std::uint64_t>(frames) || r.status().finished; });
    r.request_stop(RecStop::User);
    r.join();
    const RecorderStatus st = r.status();
    return st.frames == static_cast<std::uint64_t>(frames) && st.audio && st.error.empty();
}

struct Media {
    std::string dir;
    std::string av;      /* MJPEG 160x90 + AAC 16 kHz stereo, 3 s, a 1 kHz burst at 1.5 s */
    std::string late;    /* the burst file with its audio starting 0.2 s after the video */
    std::string tone;    /* the same with a 440 Hz sine, 1 s */
    std::string vonly;   /* video only, 2 s */
    std::string garbage; /* not a media file */
    std::string scene;   /* by VHS's Recorder, 2 s of video (30 fps) with the tone */
    bool ffmpeg = false;
};

Media make_media(const std::string &dir) {
    Media m;
    m.dir = dir;
    m.garbage = dir + "/garbage.mkv";
    write_file(m.garbage, std::string(4096, 'x') + "this is not a movie\n");
    if (!have_tool("ffmpeg")) return m;
    write_file(dir + "/burst.wav", burst_wav(16000, 3, 1.5));
    const std::string v = "-f lavfi -i testsrc2=size=160x90:rate=30 ";
    if (run_ffmpeg(v + "-i " + dir + "/burst.wav -t 3 -map 0:v -map 1:a -c:v mjpeg -q:v 12 -c:a aac -b:a 64k -ac 2 -ar 16000 " + dir +
                   "/burst.mkv"))
        m.av = dir + "/burst.mkv";
    if (run_ffmpeg(v + "-itsoffset 0.2 -i " + dir + "/burst.wav -t 3 -map 0:v -map 1:a -c:v mjpeg -q:v 12 -c:a aac -b:a 64k -ac 2 -ar 16000 " +
                   dir + "/late.mkv"))
        m.late = dir + "/late.mkv";
    if (run_ffmpeg(v + "-f lavfi -i sine=frequency=440:sample_rate=16000 -t 1 -map 0:v -map 1:a -c:v mjpeg -q:v 12 -c:a aac -b:a 64k "
                       "-ac 2 " + dir + "/tone.mkv"))
        m.tone = dir + "/tone.mkv";
    if (run_ffmpeg(v + "-t 2 -c:v mjpeg -q:v 12 " + dir + "/vonly.mkv")) m.vonly = dir + "/vonly.mkv";
    /* the Recorder's scene needs real JPEGs only for the picture; they come from the same testsrc2 */
    std::vector<std::string> jpegs;
    if (run_ffmpeg(v + "-frames:v 4 -q:v 12 -f mjpeg " + dir + "/frames.mjpg")) {
        std::ifstream in(dir + "/frames.mjpg", std::ios::binary);
        const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t at = 0;
        while (at + 4 < all.size()) {
            const std::size_t s = all.find("\xFF\xD8", at);
            if (s == std::string::npos) break;
            const std::size_t e = all.find("\xFF\xD9", s + 2);
            if (e == std::string::npos) break;
            jpegs.push_back(all.substr(s, e + 2 - s));
            at = e + 2;
        }
    }
    if (jpegs.empty()) jpegs.push_back(std::string("\xFF\xD8\xFF\xD9", 4));
    if (make_scene(dir, "scene_001_nodate", 60, jpegs)) m.scene = dir + "/scene_001_nodate.mkv";
    m.ffmpeg = true;
    return m;
}

/* ------------------------------------------------------------------------------------------------ a fake output */

struct Record {
    std::mutex mu;
    std::vector<int16_t> samples; /* every S16 sample written, all streams */
    int opens = 0, drains = 0, drops = 0, closes = 0, provider_opens = 0, writes = 0;
    std::vector<uint32_t> rates;
    std::vector<std::string> targets; /* the OutputTarget keys the provider was asked for */
    long fail_after = -1;   /* writes before -ENODEV */
    bool still_there = false;
    double speed = 0;       /* 0: instant; else frames per second heard x speed (a paced output) */
    long fixed_delay = 0;
    int gate = -1;          /* writes allowed before the sink blocks (-1: never) */
    int open_error = 0;     /* the provider's open fails with this (and open_why) */
    std::string open_why;
    std::string pulse_name; /* not empty: the sink is a sound server's, with this name */
};

class FakeSink final : public PlaybackSink {
public:
    explicit FakeSink(Record &r) : r_(r) {}
    int open(const AudioFormat &src, SinkFormat &g) override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.opens;
        r_.rates.push_back(src.rate);
        g.rate = src.rate;
        g.channels = src.channels;
        g.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        fmt_ = g;
        start_ = std::chrono::steady_clock::now();
        written_ = 0;
        return 0;
    }
    long write(const void *f, size_t n) override {
        {
            std::unique_lock<std::mutex> l(r_.mu);
            if (r_.fail_after == 0) {
                if (r_.still_there) r_.fail_after = -1; /* a profile switch: the next stream works */
                return -ENODEV;
            }
            if (r_.gate >= 0 && r_.writes >= r_.gate) { /* blocked: the buffer is full */
                l.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return 0;
            }
            if (r_.fail_after > 0) --r_.fail_after;
            ++r_.writes;
            if (fmt_.format == SampleFormat::S16_LE) {
                const int16_t *s = static_cast<const int16_t *>(f);
                r_.samples.insert(r_.samples.end(), s, s + n * fmt_.channels);
            }
        }
        if (r_.speed > 0) {
            while (static_cast<double>(written_) > heard() + fmt_.rate * 0.05)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        written_ += n;
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override {
        if (r_.speed > 0) return written_ > heard() ? static_cast<long>(written_ - static_cast<uint64_t>(heard())) : 0;
        return std::min<long>(r_.fixed_delay, static_cast<long>(written_));
    }
    void drop() override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.drops;
        written_ = 0;
        start_ = std::chrono::steady_clock::now();
    }
    void drain() override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.drains;
        written_ = 0;
    }
    void close() override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.closes;
    }

private:
    double heard() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() * fmt_.rate * r_.speed;
    }
    Record &r_;
    SinkFormat fmt_;
    std::chrono::steady_clock::time_point start_;
    uint64_t written_ = 0;
};

class FakeProvider final : public SinkProvider {
public:
    explicit FakeProvider(Record &r) : r_(r) {}
    int open(const OutputTarget &t, OpenedSink &o, std::string &why) override {
        {
            std::lock_guard<std::mutex> l(r_.mu);
            ++r_.provider_opens;
            r_.targets.push_back(t.key());
            if (r_.open_error) {
                why = r_.open_why;
                return r_.open_error;
            }
        }
        o.sink = std::make_unique<FakeSink>(r_);
        o.name = !r_.pulse_name.empty() ? r_.pulse_name : (t.kind == OutputTarget::Kind::Auto ? "default" : t.id);
        o.label = "Fake output";
        o.device = o.name;
        o.pulse = !r_.pulse_name.empty();
        return 0;
    }
    bool still_there(const OpenedSink &) override { return r_.still_there; }

private:
    Record &r_;
};

bool wait_engine(Engine &e, EngineEvent::Kind kind, std::vector<EngineEvent> &seen, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        EngineEvent ev;
        while (e.poll_event(ev)) {
            seen.push_back(ev);
            if (ev.kind == kind) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool wait_state(Engine &e, PlayState s, int ms = 2000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (e.snapshot().state == s) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* ------------------------------------------------------------------------------------------------ the engine */

void engine_tests(const std::string &dir) {
    write_file(dir + "/a.wav", ramp_wav(44100, 5000, 0));
    write_file(dir + "/b.wav", ramp_wav(44100, 3000, 5000));
    write_file(dir + "/c.wav", ramp_wav(48000, 2000, 20000));
    write_file(dir + "/long.wav", ramp_wav(44100, 44100 * 4, 0)); /* 4 s */

    /* gapless (left in Vinyl's engine, inert in VHS: nothing is named next): a then b on one stream, the samples back to back */
    {
        Record r;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        e.set_next(1, dir + "/b.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::QueueEnd, seen));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 1);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(8000 * 2));
        bool exact = r.samples.size() == 16000;
        for (size_t i = 0; exact && i < 8000; ++i)
            exact = r.samples[2 * i] == static_cast<int16_t>(i) && r.samples[2 * i + 1] == static_cast<int16_t>(-static_cast<int>(i));
        CHECK(exact);
        CHECK_EQ(r.drains, 1); /* only at the end of the queue */
        const EngineSnapshot s = e.snapshot();
        CHECK(s.state == PlayState::Paused);
        CHECK_EQ(s.pos, 1);
        CHECK_EQ(s.frame, static_cast<uint64_t>(0)); /* back at the start of the last track */
    }
    /* the end of a single file: QueueEnd, drained once, paused back at its start */
    {
        Record r;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::QueueEnd, seen));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(5000 * 2));
        CHECK_EQ(r.drains, 1);
        const EngineSnapshot s = e.snapshot();
        CHECK(s.state == PlayState::Paused);
        CHECK_EQ(s.frame, static_cast<uint64_t>(0));
        CHECK_EQ(s.total, static_cast<uint64_t>(5000));
    }
    /* another rate: the first stream is drained, a new one opened at 48 kHz */
    {
        Record r;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        e.set_next(1, dir + "/c.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::QueueEnd, seen));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 2);
        CHECK(r.rates.size() == 2 && r.rates[0] == 44100 && r.rates[1] == 48000);
        CHECK_EQ(r.drains, 2);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(7000 * 2));
    }
    /* pause, play, seek on a paced output (WAV seeks are exact: the demuxer lands on the target) */
    {
        Record r;
        r.speed = 1.0;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/long.wav", 1000, false);
        CHECK(wait_state(e, PlayState::Paused));
        sleep_ms(50);
        EngineSnapshot s = e.snapshot();
        CHECK_EQ(s.frame, static_cast<uint64_t>(44100)); /* loaded paused at 1 s */
        CHECK_EQ(s.total, static_cast<uint64_t>(44100 * 4));
        CHECK_EQ(r.provider_opens, 0);                    /* nothing opened while paused */
        e.play();
        sleep_ms(400);
        s = e.snapshot();
        CHECK(s.state == PlayState::Playing);
        CHECK(s.frame > 44100 + 4410 && s.frame < 44100 + 44100);
        CHECK(s.at_us > 0);
        e.pause();
        CHECK(wait_state(e, PlayState::Paused));
        const uint64_t held = e.snapshot().frame;
        sleep_ms(150);
        CHECK_EQ(e.snapshot().frame, held); /* the heard frame is kept */
        e.seek_ms(3000);
        sleep_ms(60);
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(3 * 44100));
        CHECK_EQ(e.snapshot().seeks, static_cast<uint64_t>(1));
        e.play();
        sleep_ms(200);
        CHECK(e.snapshot().frame > static_cast<uint64_t>(3 * 44100));
        e.stop();
        CHECK(wait_state(e, PlayState::Paused));
        sleep_ms(30);
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(0));
        /* unload (VHS): the output closed, the state Idle */
        e.play();
        sleep_ms(100);
        e.unload();
        CHECK(wait_state(e, PlayState::Idle));
    }
    /* the heard position is the frames written minus the output's delay: a sink that takes five periods and then blocks */
    {
        Record r;
        r.gate = 5;
        r.fixed_delay = 4000;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end && e.snapshot().frame != 5 * Engine::kPeriodFrames - 4000) sleep_ms(5);
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(5 * Engine::kPeriodFrames - 4000));
        CHECK(e.snapshot().state == PlayState::Playing);
        e.pause(); /* the output is dropped at the heard frame, nothing is lost: the position holds */
        CHECK(wait_state(e, PlayState::Paused));
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(5 * Engine::kPeriodFrames - 4000));
    }
    /* the output goes away: paused at the frame heard, OutputLost, no other output opened */
    {
        Record r;
        r.speed = 4.0;
        r.fail_after = 20;
        r.still_there = false;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::OutputLost, seen, 6000));
        const EngineSnapshot s = e.snapshot();
        CHECK(s.state == PlayState::Paused);
        CHECK(s.frame > 0 && s.frame < static_cast<uint64_t>(44100 * 4));
        CHECK_EQ(r.provider_opens, 1);
        CHECK_EQ(seen.back().text, std::string("Fake output"));
    }
    /* a Bluetooth profile switch: the same device is back within 2.5 s, playback goes on */
    {
        Record r;
        r.speed = 4.0;
        r.fail_after = 10;
        r.still_there = true;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        sleep_ms(1200);
        EngineEvent ev;
        bool lost = false;
        while (e.poll_event(ev))
            if (ev.kind == EngineEvent::Kind::OutputLost) lost = true;
        CHECK(!lost);
        CHECK(e.snapshot().state == PlayState::Playing);
        CHECK_EQ(r.provider_opens, 2);
    }
    /* the next track cannot be opened: reported as automatic, the engine idle at it */
    {
        Record r;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/b.wav", 0, true);
        e.set_next(1, dir + "/missing.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::OpenFailed, seen));
        CHECK(seen.back().automatic);
        CHECK_EQ(seen.back().pos, 1);
        CHECK(!seen.back().text.empty());
        CHECK(wait_state(e, PlayState::Idle));
        /* a load of a missing file: OpenFailed, not automatic */
        e.load(3, dir + "/none.wav", 0, true);
        seen.clear();
        CHECK(wait_engine(e, EngineEvent::Kind::OpenFailed, seen));
        CHECK(!seen.back().automatic);
        CHECK_EQ(seen.back().err, -ENOENT);
    }
    /* the output chosen while playing: the stream is reopened on the new one */
    {
        Record r;
        r.speed = 2.0;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        sleep_ms(150);
        OutputTarget t;
        t.kind = OutputTarget::Kind::Pulse;
        t.id = "other";
        e.set_output(t);
        sleep_ms(200);
        CHECK_EQ(r.provider_opens, 2);
        CHECK_EQ(e.snapshot().output_name, std::string("other"));
        CHECK(e.snapshot().state == PlayState::Playing);
    }
    /* the output cannot be opened at play: OutputFailed, paused */
    {
        Record r;
        r.open_error = -ENODEV;
        r.open_why = "no device";
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        std::vector<EngineEvent> seen;
        CHECK(wait_engine(e, EngineEvent::Kind::OutputFailed, seen));
        CHECK_EQ(seen.back().text, std::string("no device"));
        CHECK(wait_state(e, PlayState::Paused));
    }
}

/* ------------------------------------------------------------------------------------------------ the decoder */

void decoder_tests(const Media &m) {
    /* a library getter that gives nothing: kNeedsFfmpeg */
    {
        TrackInfo info;
        int err = 0;
        CHECK(open_track(m.av, DecodeLibs(), info, &err) == nullptr);
        CHECK_EQ(err, kNeedsFfmpeg);
        CHECK(!track_error_text(err, m.av).empty());
    }
    const DecodeLibs libs = real_decode_libs();
    {
        TrackInfo info;
        int err = 0;
        CHECK(open_track(m.dir + "/does-not-exist.mkv", libs, info, &err) == nullptr);
        CHECK_EQ(err, -ENOENT);
        CHECK(track_error_text(err, "x").find("gone") != std::string::npos);
        err = 0;
        CHECK(open_track(m.garbage, libs, info, &err) == nullptr);
        CHECK_EQ(err, kNotAudio);
    }
    if (m.av.empty()) {
        SKIP("playaudio decoder: ffmpeg could not make the MKV fixture");
        return;
    }
    {
        TrackInfo info;
        int err = -1;
        std::unique_ptr<TrackDecoder> d = open_track(m.av, libs, info, &err);
        CHECK(d != nullptr);
        CHECK_EQ(err, 0);
        if (!d) return;
        CHECK_EQ(info.out.rate, 16000u);
        CHECK_EQ(info.out.channels, static_cast<uint16_t>(2));
        CHECK_EQ(info.out.bits, static_cast<uint16_t>(16));
        CHECK_EQ(info.label, std::string("AAC"));
        CHECK(info.lossy);
        CHECK(info.frames_known); /* the stream's duration, else the container's */
        CHECK_NEAR(static_cast<double>(info.total_frames) / 16000.0, 3.0, 0.2);
        /* read to the end: the whole 3 s (AAC pads to its 1024-sample frames) */
        std::vector<int16_t> buf(2048 * 2);
        uint64_t total = 0;
        long n;
        while ((n = d->read(buf.data(), 2048)) > 0) total += static_cast<uint64_t>(n);
        CHECK_EQ(n, 0L);
        CHECK_NEAR(static_cast<double>(total) / 16000.0, 3.0, 0.15);
        CHECK_EQ(d->position(), total);
        /* a seek goes through the video's index and lands on the target (the samples before it are dropped) */
        CHECK_EQ(d->seek(32000), 0);
        CHECK_EQ(d->position(), static_cast<uint64_t>(32000)); /* 2.0 s */
        uint64_t rest = 0;
        while ((n = d->read(buf.data(), 2048)) > 0) rest += static_cast<uint64_t>(n);
        CHECK_NEAR(static_cast<double>(rest) / 16000.0, 1.0, 0.15);
        CHECK_EQ(d->seek(0), 0);
        CHECK_EQ(d->position(), static_cast<uint64_t>(0));
        CHECK(d->read(buf.data(), 2048) > 0);
        /* a seek past the end reads nothing more, as a track that ended */
        CHECK_EQ(d->seek(16000ull * 10), 0);
        CHECK_EQ(d->read(buf.data(), 2048), 0L);
    }
    /* a seek's landing is exact to a few milliseconds: the burst at 1.5 s is where the position says */
    {
        TrackInfo info;
        int err = 0;
        std::unique_ptr<TrackDecoder> d = open_track(m.av, libs, info, &err);
        CHECK(d != nullptr);
        if (d) {
            auto onset = [&](uint64_t from_frame) {
                CHECK_EQ(d->seek(from_frame), 0);
                std::vector<int16_t> buf(2048 * 2);
                uint64_t at = d->position();
                long n;
                while ((n = d->read(buf.data(), 2048)) > 0) {
                    for (long i = 0; i < n; ++i)
                        if (std::abs(buf[static_cast<size_t>(i * 2)]) > 3000) return static_cast<long long>(at + static_cast<uint64_t>(i));
                    at += static_cast<uint64_t>(n);
                }
                return -1LL;
            };
            const long long whole = onset(0);
            /* 1.5 s on the clock (the AAC priming before 0 is dropped): within 30 ms of it, the position's own truth */
            CHECK_NEAR(static_cast<double>(whole), 24000.0, 480.0);
            for (uint64_t from : {8000ull, 16000ull, 22000ull}) {
                const long long o = onset(from);
                CHECK_NEAR(static_cast<double>(o), static_cast<double>(whole), 160.0); /* the same place, however it was reached */
            }
        }
    }
    /* a video-only file: opens, has no audio stream: not an error */
    if (!m.vonly.empty()) {
        TrackInfo info;
        int err = 0;
        CHECK(open_track(m.vonly, libs, info, &err) == nullptr);
        CHECK_EQ(err, kNoAudioStream);
    } else {
        SKIP("playaudio decoder: no video-only fixture");
    }
    /* a scene made by VHS's own Recorder */
    if (!m.scene.empty()) {
        TrackInfo info;
        int err = 0;
        std::unique_ptr<TrackDecoder> d = open_track(m.scene, libs, info, &err);
        CHECK(d != nullptr);
        if (d) {
            CHECK_EQ(info.out.rate, 16000u);
            CHECK_EQ(info.out.channels, static_cast<uint16_t>(2));
            CHECK(info.frames_known);
            CHECK_NEAR(static_cast<double>(info.total_frames) / 16000.0, 2.0, 0.2);
            CHECK_EQ(d->seek(16000), 0);
            CHECK_EQ(d->position(), static_cast<uint64_t>(16000));
            std::vector<int16_t> buf(2048 * 2);
            uint64_t rest = 0;
            long n;
            while ((n = d->read(buf.data(), 2048)) > 0) rest += static_cast<uint64_t>(n);
            CHECK_NEAR(static_cast<double>(rest) / 16000.0, 1.0, 0.2);
        }
    } else {
        SKIP("playaudio decoder: the Recorder's scene could not be made");
    }
}

/* ------------------------------------------------------------------------------------------------ the facade */

std::unique_ptr<AudioPlayerDeps> fake_deps(Record &r, std::unique_ptr<AlsaApi> alsa = nullptr) {
    auto d = std::make_unique<AudioPlayerDeps>();
    d->sinks = std::make_unique<FakeProvider>(r);
    d->alsa = std::move(alsa);
    return d;
}

bool wait_clock(AudioPlayer &p, int ms, const std::function<bool(const AudioClock &)> &pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred(p.clock())) return true;
        sleep_ms(5);
    }
    return pred(p.clock());
}

/* the first frame whose left sample is over the threshold */
long first_loud(const std::vector<int16_t> &s, int threshold) {
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        if (std::abs(s[i]) > threshold) return static_cast<long>(i / 2);
    return -1;
}

int peak(const std::vector<int16_t> &s) {
    int p = 0;
    for (int16_t v : s) p = std::max(p, std::abs(static_cast<int>(v)));
    return p;
}

/* a card's mixer controls, as snd_hctl lists them, with their values (Vinyl's test fake) */
class MixerAlsa final : public AlsaApi {
public:
    std::vector<CtlInfo> ctls;
    std::map<std::string, long> values;
    std::vector<CardInfo> card_list;
    int sets = 0;
    bool gone = false;
    bool available(std::string *) override { return true; }
    std::vector<CardInfo> cards() override { return card_list; }
    int query_capture(const std::string &, CaptureOffer &) override { return -ENOSYS; }
    int open_capture(const std::string &, const HwRequest &, std::unique_ptr<AlsaPcm> &, HwGranted &) override { return -ENOSYS; }
    int mixer_list(const std::string &, std::vector<CtlInfo> &out) override {
        if (gone) return -ENODEV;
        out = ctls;
        return 0;
    }
    int mixer_get(const std::string &, const std::string &name, long &value) override {
        if (gone) return -ENODEV;
        auto it = values.find(name);
        if (it == values.end()) return -ENOENT;
        value = it->second;
        return 0;
    }
    int mixer_set(const std::string &, const std::string &name, long value) override {
        if (gone) return -ENODEV;
        values[name] = value;
        ++sets;
        return 0;
    }
};

CtlInfo vol_ctl(const std::string &name, long min, long max, bool db = false, long dmin = 0, long dmax = 0) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.capture = name.find("Capture") != std::string::npos;
    c.count = 2;
    c.min = min;
    c.max = max;
    c.has_db = db;
    c.db_min_cdb = dmin;
    c.db_max_cdb = dmax;
    return c;
}

CtlInfo switch_ctl(const std::string &name) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.count = 2;
    c.max = 1;
    return c;
}

void player_tests(const Media &m) {
    if (m.av.empty()) {
        SKIP("playaudio player: ffmpeg could not make the MKV fixture");
        return;
    }
    /* open paused, play, pause, seek, the heard clock (a paced output: 1x) */
    {
        Record r;
        r.speed = 1.0;
        AudioPlayer p("", fake_deps(r));
        AudioClock c0 = p.clock();
        CHECK(!c0.open);
        CHECK(p.open(m.av, 0, false));
        AudioClock c = p.clock();
        CHECK(c.open && c.has_audio && !c.playing && !c.ended && !c.output_lost);
        CHECK_EQ(c.heard_ms, static_cast<int64_t>(0));
        CHECK_NEAR(c.total_ms, 3000, 200);
        CHECK(c.error.empty());
        CHECK_EQ(c.output_label, std::string("Default output")); /* the fake world has no server: ALSA's default */
        CHECK_EQ(r.provider_opens, 0); /* nothing opened while paused */
        p.play();
        CHECK(wait_clock(p, 2000, [](const AudioClock &x) { return x.playing && x.heard_ms > 0; }));
        sleep_ms(500);
        c = p.clock();
        CHECK(c.playing);
        CHECK(c.heard_ms > 250 && c.heard_ms < 1000); /* about 0.6 s of heard sound (the fake output's own pace) */
        CHECK_EQ(c.output_label, std::string("Fake output"));
        /* never backwards while playing; it runs between the engine's snapshots (a snapshot per 128 ms period at 16 kHz) */
        std::int64_t last = p.clock().heard_ms;
        int distinct = 0;
        bool backwards = false;
        for (int i = 0; i < 40; ++i) {
            sleep_ms(8);
            const std::int64_t now = p.clock().heard_ms;
            backwards = backwards || now < last;
            distinct += now != last;
            last = now;
        }
        CHECK(!backwards);
        CHECK(distinct >= 20); /* a stair of 128 ms steps would show about 3 distinct values in 320 ms */
        /* pause keeps the heard position */
        p.pause();
        CHECK(wait_clock(p, 1000, [](const AudioClock &x) { return !x.playing; }));
        sleep_ms(50);
        const std::int64_t held = p.clock().heard_ms;
        sleep_ms(200);
        CHECK_EQ(p.clock().heard_ms, held);
        /* seek while paused: the target at once, then the decoder's landing, which is the target */
        p.seek_ms(2000);
        CHECK_NEAR(p.clock().heard_ms, 2000, 70); /* the requested time at once (the engine thread has not applied it yet) */
        sleep_ms(150);
        c = p.clock();
        CHECK(!c.playing);
        CHECK_NEAR(c.heard_ms, 2000, 5); /* the decoder lands before the target and drops the samples up to it */
        /* play from there */
        p.play();
        sleep_ms(400);
        c = p.clock();
        CHECK(c.playing);
        CHECK(c.heard_ms > 2100 && c.heard_ms < 2800);
        /* a seek while playing goes on playing from the target */
        p.seek_ms(500);
        sleep_ms(300);
        c = p.clock();
        CHECK(c.playing);
        CHECK(c.heard_ms > 500 && c.heard_ms < 1000);
        /* the position can be clamped to the audio's length */
        p.seek_ms(60000);
        sleep_ms(100);
        c = p.clock();
        CHECK(c.heard_ms <= c.total_ms);
        /* the end: ended, the clock holds the length, play() starts again from the beginning */
        CHECK(wait_clock(p, 4000, [](const AudioClock &x) { return x.ended; }));
        c = p.clock();
        CHECK(c.ended && !c.playing && c.has_audio);
        CHECK_EQ(c.heard_ms, c.total_ms);
        p.play();
        sleep_ms(250);
        c = p.clock();
        CHECK(!c.ended && c.playing);
        CHECK(c.heard_ms < 600);
        /* close lets the file go */
        p.close();
        c = p.clock();
        CHECK(!c.open && !c.has_audio);
        sleep_ms(50);
        std::lock_guard<std::mutex> l(r.mu);
        CHECK(r.closes >= 1);
    }
    /* what was played: the burst is at 1.5 s, from the start and from a start_ms */
    {
        Record r;
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.av, 0, true));
        CHECK(wait_clock(p, 4000, [](const AudioClock &x) { return x.ended; }));
        long o0;
        {
            std::lock_guard<std::mutex> l(r.mu);
            o0 = first_loud(r.samples, 3000);
            CHECK(r.samples.size() / 2 > 44000);
        }
        CHECK_NEAR(static_cast<double>(o0), 24000.0, 480.0);
        Record r2;
        AudioPlayer q("", fake_deps(r2));
        CHECK(q.open(m.av, 1000, true));
        CHECK(wait_clock(q, 4000, [](const AudioClock &x) { return x.ended; }));
        long o1;
        {
            std::lock_guard<std::mutex> l(r2.mu);
            o1 = first_loud(r2.samples, 3000);
        }
        CHECK_NEAR(static_cast<double>(o1), static_cast<double>(o0 - 16000), 160.0); /* 1 s earlier in the stream */
    }
    /* audio that starts 0.2 s after the video (and its AAC priming 64 ms before that): the clock starts at the first sample
     * delivered, and the burst (at 1.7 s of movie time) comes where that clock says */
    if (!m.late.empty()) {
        Record r;
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.late, 0, false));
        AudioClock c = p.clock();
        CHECK(c.has_audio);
        CHECK(c.heard_ms >= 120 && c.heard_ms <= 210);
        const std::int64_t start_ms = c.heard_ms;
        p.seek_ms(1000);
        sleep_ms(100);
        CHECK_NEAR(p.clock().heard_ms, 1000, 5);
        p.seek_ms(0); /* before the audio: it lands on the audio's start */
        sleep_ms(100);
        CHECK_NEAR(p.clock().heard_ms, static_cast<double>(start_ms), 5);
        p.play();
        CHECK(wait_clock(p, 4000, [](const AudioClock &x) { return x.ended; }));
        std::lock_guard<std::mutex> l(r.mu);
        const long onset = first_loud(r.samples, 3000);
        CHECK_NEAR(static_cast<double>(onset), (1700.0 - static_cast<double>(start_ms)) * 16.0, 480.0);
    } else {
        SKIP("playaudio player: no late-audio fixture");
    }
    /* a video-only file: opens, no audio, no error; every call is harmless */
    if (!m.vonly.empty()) {
        Record r;
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.vonly, 0, true));
        AudioClock c = p.clock();
        CHECK(c.open);
        CHECK(!c.has_audio);
        CHECK(!c.playing);
        CHECK(c.error.empty());
        CHECK_EQ(c.heard_ms, static_cast<int64_t>(0));
        p.play();
        p.seek_ms(500);
        p.pause();
        sleep_ms(50);
        c = p.clock();
        CHECK(c.open && !c.has_audio && !c.playing && c.error.empty());
        CHECK_EQ(r.provider_opens, 0); /* no output was ever opened */
    } else {
        SKIP("playaudio player: no video-only fixture");
    }
    /* open failures: a missing file, a file that is no movie */
    {
        Record r;
        AudioPlayer p("", fake_deps(r));
        CHECK(!p.open(m.dir + "/nothing-here.mkv", 0, false));
        AudioClock c = p.clock();
        CHECK(!c.open && !c.has_audio);
        CHECK(c.error.find("gone") != std::string::npos);
        CHECK(!p.open(m.garbage, 0, false));
        c = p.clock();
        CHECK(!c.open);
        CHECK(!c.error.empty());
        /* and a good one afterwards */
        CHECK(p.open(m.av, 0, false));
        c = p.clock();
        CHECK(c.open && c.has_audio && c.error.empty());
    }
    /* FFmpeg not available: the open fails with the install sentence (the decoder getter gives nothing) */
    {
        Record r;
        auto d = fake_deps(r);
        d->libs.ffmpeg = [] { return static_cast<const FfmpegLib *>(nullptr); };
        AudioPlayer p("", std::move(d));
        CHECK(!p.open(m.av, 0, false));
        CHECK(p.clock().error.find("FFmpeg") != std::string::npos);
    }
    /* an output that cannot be opened: the audio stays, playing goes false, the error says why (the video side runs on
     * its own clock then); a second play() tries again */
    {
        Record r;
        r.open_error = -ENODEV;
        r.open_why = "no device";
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.av, 0, true));
        CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return !x.error.empty(); }));
        AudioClock c = p.clock();
        CHECK(c.open && c.has_audio && !c.playing && !c.output_lost);
        CHECK(c.error.find("cannot play") != std::string::npos);
        CHECK(c.error.find("no device") != std::string::npos);
        {
            std::lock_guard<std::mutex> l(r.mu);
            r.open_error = 0;
        }
        p.play(); /* the instant output plays the whole file at once: playing or already at the end */
        CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.playing || x.ended; }));
        CHECK(p.clock().error.empty());
    }
    /* the output goes away while playing: paused, output_lost, the name in the sentence */
    {
        Record r;
        r.speed = 4.0;
        r.fail_after = 12;
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.av, 0, true));
        CHECK(wait_clock(p, 6000, [](const AudioClock &x) { return x.output_lost; }));
        const AudioClock c = p.clock();
        CHECK(c.has_audio && !c.playing);
        CHECK(c.error.find("Fake output") != std::string::npos);
        CHECK(c.error.find("disconnected") != std::string::npos);
    }
    /* a scene made by VHS's Recorder: opens, plays to its end, seeks */
    if (!m.scene.empty()) {
        Record r;
        AudioPlayer p("", fake_deps(r));
        CHECK(p.open(m.scene, 0, false));
        AudioClock c = p.clock();
        CHECK(c.has_audio);
        CHECK_NEAR(c.total_ms, 2000, 200);
        p.seek_ms(1000);
        sleep_ms(100);
        CHECK_NEAR(p.clock().heard_ms, 1000, 70);
        p.play();
        CHECK(wait_clock(p, 4000, [](const AudioClock &x) { return x.ended; }));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_NEAR(static_cast<double>(r.samples.size() / 2) / 16000.0, 1.0, 0.2); /* the second second */
    } else {
        SKIP("playaudio player: the Recorder's scene could not be made");
    }
}

/* the shipped fake output (play/fake_output.hpp): the controller's tests and the headless scenes use it */
void fake_output_tests(const Media &m) {
    if (m.tone.empty()) {
        SKIP("playaudio fake output: ffmpeg could not make the tone fixture");
        return;
    }
    CHECK(fake_output_options("lost").lost_after_ms > 0);
    CHECK(!fake_output_options("none").open_error.empty());
    CHECK(!fake_output_options("instant").pace);
    CHECK(fake_output_options("anything").pace && fake_output_options("anything").open_error.empty());
    {   /* real time: the 1 s clip is heard in about a second, and its clock moves with it */
        AudioPlayer p("", fake_audio_deps());
        CHECK(p.open(m.tone, 0, true));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.ended; }));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(s > 0.8 && s < 2.0);
        CHECK_EQ(p.clock().output_label, std::string("Fake output"));
        CHECK(p.poll_key() == AudioPlayer::Key::None);
    }
    {   /* instant: the end at once */
        AudioPlayer p("", fake_audio_deps(fake_output_options("instant")));
        CHECK(p.open(m.tone, 0, true));
        CHECK(wait_clock(p, 1500, [](const AudioClock &x) { return x.ended; }));
    }
    {   /* none: the output cannot be opened, the audio stays */
        AudioPlayer p("", fake_audio_deps(fake_output_options("none")));
        CHECK(p.open(m.tone, 0, true));
        CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return !x.error.empty(); }));
        const AudioClock c = p.clock();
        CHECK(c.has_audio && !c.playing && c.error.find("no device") != std::string::npos);
    }
    {   /* lost: it plays, then the device goes away for good */
        FakeOutputOptions o = fake_output_options("lost");
        o.lost_after_ms = 300;
        AudioPlayer p("", fake_audio_deps(o));
        CHECK(p.open(m.av.empty() ? m.tone : m.av, 0, true));
        CHECK(wait_clock(p, 6000, [](const AudioClock &x) { return x.output_lost; }));
        CHECK(!p.clock().playing);
    }
}

/* ------------------------------------------------------------------------------------------------ the volume */

void volume_player_tests(const Media &m, const std::string &dir) {
    if (m.tone.empty()) {
        SKIP("playaudio volume: ffmpeg could not make the tone fixture");
        return;
    }
    /* the sound card's own mixer (the Apple adapter: -60..0 dB, 0.5 dB steps, with its switch) */
    {
        Record r;
        auto alsa = std::make_unique<MixerAlsa>();
        MixerAlsa *a = alsa.get();
        a->ctls = {vol_ctl("Master Playback Volume", 0, 120, true, -6000, 0), switch_ctl("Master Playback Switch")};
        a->values["Master Playback Volume"] = 120;
        a->values["Master Playback Switch"] = 1;
        AudioPlayer p("", fake_deps(r, std::move(alsa)));
        CHECK_EQ(p.volume_percent(), 100);
        CHECK(p.volume_adjustable());
        CHECK(!p.volume_muted());
        CHECK_EQ(p.volume_label(), std::string("Default output"));
        p.volume_step(-1);
        CHECK_EQ(p.volume_percent(), 97); /* level 29 of 30 */
        CHECK(a->values["Master Playback Volume"] < 120);
        p.volume_step(1);
        CHECK_EQ(p.volume_percent(), 100);
        CHECK_EQ(a->values["Master Playback Volume"], 120L);
        for (int i = 0; i < 29; ++i) p.volume_step(-1);
        CHECK_EQ(p.volume_percent(), 3); /* level 1 */
        p.volume_step(-1);               /* "-" at the lowest level mutes (the switch) */
        CHECK_EQ(a->values["Master Playback Switch"], 0L);
        CHECK(p.volume_muted());
        CHECK_EQ(p.volume_percent(), 0);
        p.volume_step(-1); /* muted: "-" does nothing */
        CHECK_EQ(a->values["Master Playback Switch"], 0L);
        p.volume_step(1); /* "+" brings the sound back at level 1 */
        CHECK_EQ(a->values["Master Playback Switch"], 1L);
        CHECK(!p.volume_muted());
        CHECK_EQ(p.volume_percent(), 3);
        /* the sound card went away: no mixer any more (the software gain takes over); no crash */
        a->gone = true;
        sleep_ms(350);
        p.volume_step(1);
        (void)p.volume_percent();
    }
    /* G6, an output's first use (the Pi 4B: its 'PCM Playback Volume' at -102.39 dB, the minimum that mutes): VHS has no
     * level for it, so it starts near 50 % and the volume line says "was off"; the level is kept per output, and the
     * next time the same output is found silent (the user's choice now) it is left alone */
    {
        const std::string levels = dir + "/first-use-levels";
        auto make = [&](Record &r, MixerAlsa *&a) {
            auto alsa = std::make_unique<MixerAlsa>();
            a = alsa.get();
            CtlInfo pcm = vol_ctl("PCM Playback Volume", -10239, 400, true, -10239, 400);
            pcm.min_is_mute = true; /* the Pi 4B's control: its lowest value is mute */
            a->ctls = {pcm};
            a->values["PCM Playback Volume"] = -10239;
            auto d = fake_deps(r, std::move(alsa));
            d->volume_state_file = levels;
            return d;
        };
        {
            Record r;
            r.speed = 1;
            MixerAlsa *a = nullptr;
            AudioPlayer p("", make(r, a));
            CHECK_EQ(p.volume_percent(), 0);
            CHECK(p.open(m.tone, 0, true));
            CHECK(wait_clock(p, 2000, [](const AudioClock &x) { return x.playing; }));
            bool raised = false;
            for (int i = 0; i < 100 && !raised; ++i) {
                raised = p.volume_percent() > 0;
                sleep_ms(10);
            }
            CHECK(raised);
            CHECK(p.volume_percent() >= 45 && p.volume_percent() <= 55);
            CHECK(a->values["PCM Playback Volume"] > -10239);
            CHECK_EQ(p.volume_note(), std::string("was off"));
            p.volume_step(1); /* a press: the note goes, the level is kept */
            CHECK(p.volume_note().empty());
        }
        {
            Record r;
            r.speed = 1;
            MixerAlsa *a = nullptr;
            AudioPlayer p("", make(r, a)); /* silent again (set so elsewhere): VHS knows the output now */
            CHECK(p.open(m.tone, 0, true));
            CHECK(wait_clock(p, 2000, [](const AudioClock &x) { return x.playing; }));
            sleep_ms(400);
            CHECK_EQ(p.volume_percent(), 0);
            CHECK_EQ(a->values["PCM Playback Volume"], -10239L);
            CHECK(p.volume_note().empty());
        }
    }
    /* a card without a playback mixer: a software gain over -60..0 dB, kept per output in a small file */
    {
        const std::string levels = dir + "/output-levels";
        auto make = [&](Record &r) {
            auto d = fake_deps(r, std::make_unique<MixerAlsa>());
            d->volume_state_file = levels;
            return d;
        };
        int peak_unity = 0, peak_quiet = 0;
        {
            Record r;
            AudioPlayer p("", make(r));
            CHECK_EQ(p.volume_percent(), 100);
            CHECK(p.volume_adjustable());
            CHECK(p.open(m.tone, 0, true));
            CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.ended; }));
            std::lock_guard<std::mutex> l(r.mu);
            peak_unity = peak(r.samples);
        }
        CHECK(peak_unity > 1500);
        {
            Record r;
            AudioPlayer p("", make(r));
            for (int i = 0; i < 10; ++i) p.volume_step(-1);
            CHECK_EQ(p.volume_percent(), 67); /* level 20 of 30 */
            CHECK(p.open(m.tone, 0, true));
            CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.ended; }));
            std::lock_guard<std::mutex> l(r.mu);
            peak_quiet = peak(r.samples);
        }
        /* -20.7 dB: 0.092 of the amplitude */
        CHECK_NEAR(static_cast<double>(peak_quiet) / peak_unity, 0.092, 0.03);
        {
            Record r;
            AudioPlayer p("", make(r)); /* a restart: the level is kept */
            CHECK_EQ(p.volume_percent(), 67);
            for (int i = 0; i < 19; ++i) p.volume_step(-1);
            CHECK_EQ(p.volume_percent(), 3);
            p.volume_step(-1); /* VHS's own mute: silent, the level kept */
            CHECK(p.volume_muted());
            CHECK_EQ(p.volume_percent(), 0);
            CHECK(p.open(m.tone, 0, true));
            CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.ended; }));
            {
                std::lock_guard<std::mutex> l(r.mu);
                CHECK_EQ(peak(r.samples), 0);
            }
            p.volume_step(1);
            CHECK(!p.volume_muted());
            CHECK_EQ(p.volume_percent(), 3);
        }
        VolumeState vs;
        CHECK(load_volume_state(levels, vs));
        CHECK_EQ(app_volume_of(vs, "auto"), 3);
        CHECK(!app_muted_of(vs, "auto"));
    }
    /* a sink of the sound server (a fake world: no worker, the sink changes in memory) */
    {
        Record r;
        r.pulse_name = "alsa_output.usb-Test.analog-stereo";
        auto d = fake_deps(r);
        PulseDevice usb;
        usb.name = r.pulse_name;
        usb.description = "USB Audio Device Analog Stereo";
        usb.channels = 2;
        usb.volume_raw = 0x10000;
        usb.volume_pct = 100;
        d->world.server = true;
        d->world.sinks = {usb};
        d->world.default_sink = usb.name;
        AudioPlayer p("", std::move(d));
        CHECK_EQ(p.volume_percent(), 100);
        CHECK_EQ(p.volume_label(), std::string("USB Audio Device")); /* the device's name, not the profile words */
        CHECK(p.volume_adjustable());
        p.volume_step(-1);
        CHECK_EQ(p.volume_percent(), 97);
        for (int i = 0; i < 28; ++i) p.volume_step(-1);
        CHECK_EQ(p.volume_percent(), 3);
        p.volume_step(-1); /* the sink's mute, the level kept */
        CHECK(p.volume_muted());
        CHECK_EQ(p.volume_percent(), 0);
        p.volume_step(1);
        CHECK(!p.volume_muted());
        CHECK_EQ(p.volume_percent(), 3);
        /* playing on it: the engine's gain stays unity, the sink's own volume does the work */
        CHECK(p.open(m.tone, 0, true));
        CHECK(wait_clock(p, 3000, [](const AudioClock &x) { return x.ended; }));
        CHECK_EQ(p.clock().output_label, std::string("Fake output"));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK(peak(r.samples) > 1500);
    }
    /* a sink that reports no level: not known, nothing to change */
    {
        Record r;
        r.pulse_name = "alsa_output.x";
        auto d = fake_deps(r);
        PulseDevice s;
        s.name = r.pulse_name;
        d->world.server = true;
        d->world.sinks = {s};
        d->world.default_sink = s.name;
        AudioPlayer p("", std::move(d));
        CHECK_EQ(p.volume_percent(), -1);
        CHECK(!p.volume_adjustable());
        p.volume_step(1);
        CHECK_EQ(p.volume_percent(), -1);
    }
}

/* ------------------------------------------------------------------------------------------------ keys, outputs */

std::string key_bitmap(std::initializer_list<unsigned> codes) {
    uint64_t w[8] = {0};
    for (unsigned c : codes) w[c / 64] |= uint64_t(1) << (c % 64);
    int top = 7;
    while (top > 0 && !w[top]) --top;
    std::string s;
    char b[32];
    for (int i = top; i >= 0; --i) {
        std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(w[i]));
        s += b;
        if (i) s += ' ';
    }
    return s;
}

/* the HackBerry's nodes as read in Vinyl's report 059 (13.6): the speaker's AVRCP node (PLAYCD, PAUSECD, NEXTSONG,
 * PREVIOUSSONG, STOPCD, REWIND, FASTFORWARD, volume and mute; no PLAYPAUSE) and the keyboard's Consumer Control node */
std::string devices_text() {
    std::string t;
    t += "I: Bus=0005 Vendor=0000 Product=0000 Version=0000\nN: Name=\"XWF-M18-M28-M38 (AVRCP)\"\nH: Handlers=kbd event10\nB: EV=100013\n";
    t += "B: KEY=" + key_bitmap({200, 201, 163, 165, 166, 168, 208, 113, 114, 115}) + "\n\n";
    t += "I: Bus=0003\nN: Name=\"ZitaoTech HACKBerryPiQ20 Consumer Control\"\nH: Handlers=kbd event3\n";
    t += "B: KEY=" + key_bitmap({164, 163, 165, 166, 115, 114}) + "\n\n";
    t += "I: Bus=0018\nN: Name=\"Goodix Capacitive TouchScreen\"\nH: Handlers=mouse0 event1\nB: KEY=" + key_bitmap({330}) + "\n\n";
    return t;
}

void media_key_tests(const std::string &base) {
    CHECK(media_action_for(164) == MediaAction::Toggle); /* PLAYPAUSE */
    CHECK(media_action_for(200) == MediaAction::Toggle); /* PLAYCD */
    CHECK(media_action_for(201) == MediaAction::Toggle); /* PAUSECD */
    CHECK(media_action_for(163) == MediaAction::Next);
    CHECK(media_action_for(165) == MediaAction::Prev);
    CHECK(media_action_for(166) == MediaAction::Stop);
    CHECK(media_action_for(115) == MediaAction::None); /* volume up: the launcher's */
    const std::vector<InputDevice> devs = parse_input_devices(devices_text());
    CHECK_EQ(devs.size(), static_cast<size_t>(3));
    if (devs.size() == 3) {
        CHECK_EQ(devs[0].name, std::string("XWF-M18-M28-M38 (AVRCP)"));
        CHECK_EQ(devs[0].event, std::string("event10"));
        CHECK(devs[0].has_key(200));
        CHECK(!devs[0].has_key(164));
        CHECK(devs[0].media());
        CHECK(devs[1].has_key(164));
        CHECK(devs[1].media());
        CHECK(!devs[2].media());
    }
    /* dedupe: a press seen twice within 250 ms acts once; another action is not held back */
    Dedupe d;
    CHECK(d.accept(MediaAction::Toggle, 1000));
    CHECK(!d.accept(MediaAction::Toggle, 1100));
    CHECK(d.accept(MediaAction::Next, 1100));
    CHECK(d.accept(MediaAction::Toggle, 1300));
    CHECK(!d.accept(MediaAction::None, 5000));
    /* presses and releases: next and previous report their release from the source whose press acted */
    MediaPresses mp;
    MediaEvent e;
    CHECK(mp.feed("event10", MediaAction::Next, 1, 1000, e) && e == (MediaEvent{MediaAction::Next, true}));
    CHECK(!mp.feed("event10", MediaAction::Next, 2, 1300, e)); /* auto-repeat */
    CHECK(!mp.feed("lvgl", MediaAction::Next, 1, 1100, e));    /* the same press by another path */
    CHECK(!mp.feed("lvgl", MediaAction::Next, 0, 1150, e));    /* and its release */
    CHECK(mp.feed("event10", MediaAction::Next, 0, 1800, e) && e == (MediaEvent{MediaAction::Next, false}));
    CHECK(mp.feed("event10", MediaAction::Toggle, 1, 2000, e) && e.down);
    CHECK(!mp.feed("event10", MediaAction::Toggle, 0, 2100, e)); /* play / pause: no release */
    const std::string dir = base + "/keys";
    make_dirs(dir);
    write_file(dir + "/devices", devices_text());
    const std::string fifo = dir + "/event10";
    CHECK_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    const int wfd = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK); /* RDWR: the open does not wait for the reader */
    CHECK(wfd >= 0);
    auto send = [&](unsigned code, int value) {
        input_event ev{};
        ev.type = EV_KEY;
        ev.code = static_cast<uint16_t>(code);
        ev.value = value;
        CHECK_EQ(::write(wfd, &ev, sizeof ev), static_cast<ssize_t>(sizeof ev));
        input_event syn{};
        syn.type = EV_SYN;
        CHECK_EQ(::write(wfd, &syn, sizeof syn), static_cast<ssize_t>(sizeof syn));
    };
    /* the reader on a FIFO standing for /dev/input/event10: presses act once, repeats never, the release of next counts */
    {
        MediaKeyReader r;
        r.start(dir + "/devices", dir);
        for (int i = 0; i < 100 && r.open_nodes().empty(); ++i) sleep_ms(20);
        CHECK_EQ(r.open_nodes().size(), static_cast<size_t>(1));
        send(200, 1);
        send(200, 0);
        send(200, 2);
        send(163, 1);
        send(163, 0);
        std::vector<MediaEvent> got;
        for (int i = 0; i < 100 && got.size() < 3; ++i) {
            sleep_ms(20);
            for (const MediaEvent &a : r.take()) got.push_back(a);
        }
        CHECK_EQ(got.size(), static_cast<size_t>(3));
        if (got.size() == 3) {
            CHECK(got[0] == (MediaEvent{MediaAction::Toggle, true}));
            CHECK(got[1] == (MediaEvent{MediaAction::Next, true}));
            CHECK(got[2] == (MediaEvent{MediaAction::Next, false}));
        }
        r.stop();
    }
    /* the same through the facade: poll_key gives one press per call, never a release */
    {
        Record rec;
        auto deps = fake_deps(rec);
        deps->media_keys = true;
        deps->keys_devices_path = dir + "/devices";
        deps->keys_dev_dir = dir;
        AudioPlayer p("", std::move(deps));
        CHECK(p.poll_key() == AudioPlayer::Key::None);
        sleep_ms(600); /* the reader opens the node at its first scan */
        send(200, 1);
        send(200, 0);
        send(165, 1);
        send(165, 0);
        send(166, 1);
        std::vector<AudioPlayer::Key> keys;
        for (int i = 0; i < 100 && keys.size() < 3; ++i) {
            sleep_ms(20);
            for (AudioPlayer::Key k = p.poll_key(); k != AudioPlayer::Key::None; k = p.poll_key()) keys.push_back(k);
        }
        CHECK_EQ(keys.size(), static_cast<size_t>(3));
        if (keys.size() == 3) {
            CHECK(keys[0] == AudioPlayer::Key::PlayPause);
            CHECK(keys[1] == AudioPlayer::Key::Prev);
            CHECK(keys[2] == AudioPlayer::Key::Stop);
        }
        CHECK(p.poll_key() == AudioPlayer::Key::None);
    }
    ::close(wfd);
}

void output_tests() {
    CHECK_EQ(OutputTarget::from_key("bt:70:BF:92:53:6B:8B").id, std::string("70:BF:92:53:6B:8B"));
    CHECK(OutputTarget::from_key("pulse:alsa_output.x").kind == OutputTarget::Kind::Pulse);
    CHECK(OutputTarget::from_key("alsa:plughw:CARD=UC02,DEV=0").kind == OutputTarget::Kind::Alsa);
    CHECK(OutputTarget::from_key("weird").kind == OutputTarget::Kind::Auto);
    CHECK(OutputTarget::from_key("").kind == OutputTarget::Kind::Auto); /* Settings.output "" = Automatic */
    CHECK_EQ(OutputTarget::from_key("bt:AA").key(), std::string("bt:AA"));

    /* the chooser: Automatic (follows the system's Evolve 65), Evolve 65 in music quality, the USB card and HDMI wired, the XWF
     * speaker paired but not connected (dimmed) */
    OutputWorld w;
    w.server = true;
    PulseDevice ev;
    ev.name = "bluez_output.70_BF_92_53_6B_8B.1";
    ev.description = "Evolve 65";
    ev.bt_address = "70:BF:92:53:6B:8B";
    ev.bus = "bluetooth";
    ev.bt_profile = "a2dp-sink";
    PulseDevice usb;
    usb.name = "alsa_output.usb-UC02.analog-stereo";
    usb.description = "USB sound card UC02";
    PulseDevice hdmi;
    hdmi.name = "alsa_output.platform.hdmi-stereo";
    hdmi.description = "Built-in Audio Digital Stereo (HDMI)";
    PulseDevice mon = usb;
    mon.name += ".monitor";
    mon.is_monitor = true;
    w.sinks = {usb, ev, hdmi, mon};
    w.default_sink = ev.name;
    LauncherBtDev off;
    off.mac = "9E:0C:81:E6:5D:1A";
    off.kind = "speaker";
    LauncherBtDev on;
    on.mac = "70:BF:92:53:6B:8B";
    on.kind = "headset";
    on.connected = true;
    w.facts.devices = {on, off};
    w.facts.bt_audio = "pipewire";
    w.bt_names["9E:0C:81:E6:5D:1A"] = "XWF speaker";
    std::vector<OutputRow> rows = build_output_rows(w, OutputTarget());
    CHECK_EQ(rows.size(), static_cast<size_t>(5));
    if (rows.size() == 5) {
        CHECK_EQ(rows[0].label, std::string("Automatic"));
        CHECK_EQ(rows[0].value, std::string("follows system: Evolve 65"));
        CHECK(rows[0].chosen);
        CHECK_EQ(rows[1].label, std::string("Evolve 65"));
        CHECK_EQ(rows[1].value, std::string("music quality"));
        CHECK(rows[1].target.kind == OutputTarget::Kind::Bt);
        CHECK_EQ(rows[1].target.id, std::string("70:BF:92:53:6B:8B"));
        CHECK_EQ(rows[2].label, std::string("USB sound card UC02"));
        CHECK_EQ(rows[2].value, std::string("wired"));
        CHECK_EQ(rows[3].label, std::string("HDMI"));
        CHECK_EQ(rows[4].label, std::string("XWF speaker"));
        CHECK_EQ(rows[4].value, std::string("paired, not connected"));
        CHECK(rows[4].dimmed);
    }
    CHECK(!bluetooth_not_set_up(w));
    /* call quality: a headset in its call profile */
    w.sinks[1].bt_profile = "headset-head-unit";
    rows = build_output_rows(w, OutputTarget::from_key("bt:70:BF:92:53:6B:8B"));
    CHECK_EQ(rows[1].value, std::string("call quality"));
    CHECK(rows[1].chosen);
    CHECK(!rows[0].chosen);
    /* a choice made earlier that is gone stays listed, dimmed */
    rows = build_output_rows(w, OutputTarget::from_key("bt:11:22:33:44:55:66"));
    CHECK(rows.back().chosen && rows.back().dimmed);
    /* a Lite board: ALSA cards, no Bluetooth */
    OutputWorld lite;
    CardInfo c;
    c.id = "UC02";
    c.name = "USB PnP Sound Device";
    c.playback = true;
    CardInfo cap;
    cap.id = "Mic";
    cap.capture = true;
    lite.alsa_cards = {c, cap};
    rows = build_output_rows(lite, OutputTarget());
    CHECK_EQ(rows.size(), static_cast<size_t>(2));
    CHECK_EQ(rows[0].value, std::string("system default"));
    CHECK_EQ(rows[1].target.id, std::string("plughw:CARD=UC02,DEV=0"));
    CHECK(bluetooth_not_set_up(lite));
    w.facts.bt_audio = "none";
    CHECK(bluetooth_not_set_up(w));

    /* the same through the facade, and the choice applied to the engine's next open */
    {
        Record r;
        auto alsa = std::make_unique<MixerAlsa>();
        alsa->card_list = {c, cap};
        AudioPlayer p("alsa:plughw:CARD=UC02,DEV=0", fake_deps(r, std::move(alsa)));
        CHECK_EQ(p.output_key(), std::string("alsa:plughw:CARD=UC02,DEV=0"));
        std::vector<AudioOutputRow> list = p.output_rows();
        CHECK_EQ(list.size(), static_cast<size_t>(2));
        if (list.size() == 2) {
            CHECK_EQ(list[0].key, std::string("auto"));
            CHECK_EQ(list[0].label, std::string("Automatic"));
            CHECK(!list[0].chosen);
            CHECK_EQ(list[1].key, std::string("alsa:plughw:CARD=UC02,DEV=0"));
            CHECK_EQ(list[1].label, std::string("USB PnP Sound Device"));
            CHECK(list[1].chosen);
        }
        CHECK(p.bluetooth_not_set_up());
        CHECK_EQ(p.volume_label(), std::string("USB PnP Sound Device")); /* the chosen row's label, nothing open yet */
        p.set_output("auto");
        CHECK_EQ(p.output_key(), std::string("auto"));
        list = p.output_rows();
        CHECK(!list.empty() && list[0].chosen);
        p.set_output("");
        CHECK_EQ(p.output_key(), std::string("auto"));
    }
}

/* ------------------------------------------------------------------------------------------------ the volume model */

void volume_model_tests() {
    /* an output whose range is not known (Bluetooth, software sinks): -60..0 dB, 30 levels of 60 / 29 dB */
    const OutputVolumeModel d = default_volume_model();
    CHECK_NEAR(d.lo_db, -60.0, 1e-9);
    CHECK_NEAR(d.hi_db, 0.0, 1e-9);
    CHECK_EQ(d.n, 30);
    CHECK_NEAR(d.step_db(), 60.0 / 29, 1e-9);
    CHECK_EQ(d.pct_of(1), 3);
    CHECK_EQ(d.pct_of(15), 50);
    CHECK_EQ(d.pct_of(30), 100);
    for (int i = 1; i <= d.n; ++i) {
        CHECK_EQ(d.nearest(d.db_of(i)), i);
        CHECK_EQ(d.from_pct(d.pct_of(i)), i);
    }
    CHECK_EQ(volume_model(-45, 0).n, 23);
    CHECK_EQ(volume_model(-10, 0).n, 5);
    CHECK_EQ(sink_volume_for_db(0), 0x10000u);
    CHECK_EQ(sink_volume_for_db(-60), 6554u);
    for (int i = 1; i <= d.n; ++i) CHECK_EQ(sink_level_for_volume(d, sink_volume_for_db(d.db_of(i))), i);
    CHECK_EQ(sink_level_for_volume(d, 0), 0);
    CHECK_NEAR(volume_gain_db(0.0), 1.0, 1e-9);
    CHECK_NEAR(volume_gain_db(-20.0), 0.1, 1e-6);
    CHECK_EQ(volume_gain_db(-1e9), 0.0f);
    CHECK_EQ(pulse_alsa_card("1", "", ""), 1);
    CHECK_EQ(pulse_alsa_card("", "", "hw:CARD=Device"), -1);

    /* the card behind an ALSA device, the choice of the playback control */
    CHECK_EQ(mixer_card_for("plughw:CARD=UC02,DEV=0"), std::string("hw:CARD=UC02"));
    CHECK_EQ(mixer_card_for("default"), std::string("default"));
    MixerChoice mc;
    CHECK(choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), vol_ctl("PCM Playback Volume", 0, 45),
                                 vol_ctl("Master Playback Volume", 0, 87)},
                                mc));
    CHECK_EQ(mc.volume, std::string("Master Playback Volume"));
    CHECK(!choose_playback_mixer({vol_ctl("Mic Capture Volume", 0, 30), switch_ctl("Mic Capture Switch")}, mc));

    /* The Pi 4B (Vinyl 2026-11-13): 'PCM Playback Volume' -10239..400 in hundredths of a dB with its mute flag. The ALSA layer
     * merged into VHS (audio/alsa) puts the real dB of min back: the jack gets its top 60 dB, -56..+4 dB, 30 levels. */
    CtlInfo pcm = vol_ctl("PCM Playback Volume", -10239, 400, true, kAlsaDbGainMute, 400); /* as get_dB_range says */
    pcm.count = 1;
    {
        MixerChoice bad;
        CHECK(choose_playback_mixer({pcm}, bad));
        CHECK(!bad.has_db); /* unfixed, the sentinel is never taken for a dB value */
    }
    CtlInfo fixed = pcm;
    alsa_fix_mute_db(fixed, true, -10238);
    CHECK(fixed.has_db);
    CHECK(fixed.min_is_mute);
    CHECK_EQ(fixed.db_min_cdb, -10239L);
    CHECK_EQ(fixed.db_max_cdb, 400L);
    CtlInfo nodb = pcm;
    alsa_fix_mute_db(nodb, false, 0);
    CHECK(!nodb.has_db);
    CHECK(nodb.min_is_mute);
    CtlInfo plain = vol_ctl("PCM Playback Volume", 0, 45, true, -4500, 0); /* no mute flag: left alone */
    alsa_fix_mute_db(plain, true, -4400);
    CHECK(!plain.min_is_mute);
    CHECK_EQ(plain.db_min_cdb, -4500L);
    const std::string card = "hw:CARD=Headphones";
    MixerAlsa a;
    a.ctls = {fixed, switch_ctl("PCM Playback Switch")};
    a.values["PCM Playback Volume"] = 393;
    a.values["PCM Playback Switch"] = 1;
    std::string chosen;
    MixerChoice m;
    MixerLevel lv;
    CHECK_EQ(read_mixer(a, card, chosen, m, lv), 0);
    CHECK(m.has_db && m.min_is_mute);
    CHECK_EQ(m.model.n, 30);
    CHECK_EQ(lv.level, 30);
    CHECK_EQ(lv.pct, 100);
    for (int i = 1; i <= 30; ++i) {
        const long raw = mixer_raw_for_level(m, i);
        CHECK(raw >= -5600 && raw <= 400);
        CHECK_EQ(mixer_level_for_raw(m, raw), i);
    }
    auto press = [&](int dir) {
        const VolumePress p = mixer_press(m, lv, dir);
        if (p.kind == VolumePress::Kind::Set) write_mixer_level(a, card, m, p.level);
        if (p.kind == VolumePress::Kind::Mute) write_mixer_switch(a, card, m, false);
        if (p.kind == VolumePress::Kind::Unmute) write_mixer_switch(a, card, m, true);
        read_mixer(a, card, chosen, m, lv);
        return p;
    };
    CHECK(press(-1).level == 29);
    CHECK_EQ(a.values["PCM Playback Volume"], 193L); /* +4 - 2.07 = 1.93 dB */
    for (int i = 0; i < 28; ++i) press(-1);
    CHECK_EQ(lv.level, 1);
    CHECK_EQ(a.values["PCM Playback Volume"], -5600L); /* -56 dB: quiet, not the 80-100 % the old build gave */
    CHECK(press(-1).kind == VolumePress::Kind::Mute);
    CHECK(lv.muted);
    CHECK(press(1).kind == VolumePress::Kind::Unmute);
    CHECK(!lv.muted);
    a.values["PCM Playback Volume"] = -10239; /* the control at its mute value: shown muted, level 0 */
    read_mixer(a, card, chosen, m, lv);
    CHECK(lv.muted);
    CHECK_EQ(lv.level, 0);
    const VolumePress up = press(1);
    CHECK(up.kind == VolumePress::Kind::Set && up.level == 1);
    CHECK_EQ(volume_model_text(m.model), std::string("-56.00..4.00 dB, 30 levels of 2.07 dB"));

    /* a card without a mixer: VHS's own software level and mute, kept in VolumeState */
    VolumeState s;
    const std::string key = "alsa:plughw:CARD=vc4hdmi0,DEV=0";
    bool muted = false;
    CHECK_EQ(software_level(s, key, &muted), 30);
    CHECK(!muted);
    for (int i = 0; i < 29; ++i) software_press(s, key, -1);
    CHECK_EQ(software_level(s, key, &muted), 1);
    CHECK(software_press(s, key, -1).kind == VolumePress::Kind::Mute);
    CHECK(app_muted_of(s, key));
    CHECK(software_press(s, key, -1).kind == VolumePress::Kind::None); /* muted: "-" does nothing */
    CHECK(software_press(s, key, 1).kind == VolumePress::Kind::Unmute);
    CHECK_EQ(software_level(s, key, &muted), 1);
    CHECK(!muted);
    /* the small file: a restart keeps levels and mutes; damaged or missing means a fresh start; bounded */
    set_app_volume(s, "bt:A", 40);
    set_app_muted(s, "bt:A", true);
    VolumeState back;
    CHECK(volume_state_from_text(volume_state_to_text(s), back));
    CHECK_EQ(app_volume_of(back, "bt:A"), 40);
    CHECK(app_muted_of(back, "bt:A"));
    CHECK_EQ(app_volume_of(back, key), 3);
    CHECK_EQ(app_volume_of(back, "never set"), 100);
    CHECK(!volume_state_from_text("not a levels file\n", back));
    CHECK(back.app_volumes.empty());
    CHECK(volume_state_from_text("v1\n50\tm\tk1\nbad line\n300\tu\tk2\nx\tu\tk3\n", back));
    CHECK_EQ(app_volume_of(back, "k1"), 50);
    CHECK(app_muted_of(back, "k1"));
    CHECK_EQ(app_volume_of(back, "k2"), 100); /* clamped */
    CHECK_EQ(back.app_volumes.count("k3"), static_cast<size_t>(0));
    VolumeState many;
    for (int i = 0; i < 50; ++i) set_app_volume(many, "k" + std::to_string(i), i);
    CHECK(many.app_volumes.size() <= kMaxAppVolumes);
}

/* ------------------------------------------------------------------------------------------------ the launcher */

class FakeIo final : public FactsIo {
public:
    std::map<std::string, std::string> files;
    bool alive = true;
    int gen = 0;
    bool stat(const std::string &p, std::string &stamp) override {
        auto it = files.find(p);
        if (it == files.end()) return false;
        stamp = std::to_string(gen) + ":" + std::to_string(it->second.size());
        return true;
    }
    bool read(const std::string &p, std::string &text, size_t) override {
        auto it = files.find(p);
        if (it == files.end()) return false;
        text = it->second;
        return true;
    }
    bool pid_alive(long) override { return alive; }
};

/* a launcher.sock that records every request line and answers "ok" */
struct FakeSocket {
    int fd = -1;
    std::string path;
    std::mutex mu;
    std::vector<std::string> heard;
    std::atomic<bool> quit{false};
    std::thread th;
    bool start(const std::string &dir) {
        path = dir + "/launcher.sock";
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 8) != 0) return false;
        th = std::thread([this] {
            while (!quit.load()) {
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, 50) != 1) continue;
                const int c = ::accept(fd, nullptr, nullptr);
                if (c < 0) continue;
                char b[256] = {0};
                const ssize_t n = ::recv(c, b, sizeof b - 1, 0);
                if (n > 0) {
                    std::lock_guard<std::mutex> g(mu);
                    heard.emplace_back(b, static_cast<size_t>(n));
                }
                const char *reply = "ok\n";
                (void)::send(c, reply, std::strlen(reply), 0);
                ::close(c);
            }
        });
        return true;
    }
    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> g(mu);
        return heard;
    }
    ~FakeSocket() {
        quit.store(true);
        if (th.joinable()) th.join();
        if (fd >= 0) ::close(fd);
    }
};

void launcher_tests(const std::string &base) {
    LauncherStatus s;
    parse_launcher_status("v=1\npid=826\ngen=4\nvolume=60\nmute=0\nbt=connected\nbt_audio_dev=70_BF_92_53_6B_8B:headset:0;"
                          "9E_0C_81_E6_5D_1A:speaker:1:a2dp\n",
                          true,
                          "v=1\npid=826\nbattery=1\nbacklight=levels\nsound=1\nbt_audio=pipewire\nbt_hfp=1\nrequests=hello,caps,toast,beep,volume,brightness,dim\n",
                          true, s);
    CHECK(s.have_status && s.have_board);
    CHECK_EQ(s.volume, 60);
    CHECK(s.battery_board());
    CHECK(s.has_request("dim"));
    CHECK(s.has_request("volume"));
    CHECK(!s.has_request("di"));
    CHECK(!s.has_request(""));
    CHECK_EQ(s.facts.devices.size(), static_cast<size_t>(2));
    CHECK(s.facts.stack_server());
    parse_launcher_status("v=1\npid=1\nvolume=-1\nmute=1\n", true, "", false, s);
    CHECK(!s.has_request("dim")); /* no board.state: no request */
    parse_launcher_status("", false, "v=1\npid=1\nbattery=1\nbacklight=levels\nrequests=hello,caps,toast,beep\n", true, s);
    CHECK(!s.has_request("dim")); /* the launcher does not answer dim here: nothing is sent */

    /* the reader: re-read on change, untrusted when the launcher's pid is gone */
    auto io = std::make_unique<FakeIo>();
    FakeIo *raw = io.get();
    raw->files["/s"] = "v=1\npid=5\nvolume=30\n";
    LauncherStatusReader r(std::move(io), "/s", "/b");
    CHECK(r.poll(1000));
    CHECK_EQ(r.status().volume, 30);
    CHECK(!r.poll(1500)); /* at most once a second */
    raw->files["/s"] = "v=1\npid=5\nvolume=35\n";
    raw->gen++;
    CHECK(r.poll(2100));
    CHECK_EQ(r.status().volume, 35);
    raw->alive = false;
    CHECK(r.poll(3200));
    CHECK(!r.status().have_status);

    /* launcher.sock: one line out, one line back; no socket = -1 at once */
    CHECK_EQ(launcher_call("", "dim allow", nullptr), -1);
    const std::string dir = base + "/sock";
    make_dirs(dir);
    CHECK_EQ(launcher_call(dir + "/launcher.sock", "dim allow", nullptr), -1);
    FakeSocket sock;
    CHECK(sock.start(dir));
    std::string reply;
    CHECK_EQ(launcher_call(sock.path, "dim allow", &reply, 1000), 0);
    CHECK_EQ(reply, std::string("ok"));
    CHECK(!sock.lines().empty() && sock.lines()[0] == "dim allow\n");

    /* LauncherLink: request_async never blocks; `dim` and `volume` go out only where board.state lists them */
    {
        auto fio = std::make_unique<FakeIo>();
        FakeIo *f = fio.get();
        f->files["/s"] = "v=1\npid=5\nvolume=30\n";
        f->files["/b"] = "v=1\npid=5\nbattery=1\nbacklight=levels\nrequests=hello,caps,toast,beep,volume,brightness,dim\n";
        LauncherLink link(sock.path, std::move(fio), "/s", "/b");
        CHECK(link.supports("dim"));
        CHECK(link.supports("volume"));
        CHECK(!link.supports("fly"));
        CHECK_EQ(link.status().volume, 30);
        const size_t before = sock.lines().size();
        link.request_async("dim deny");
        CHECK(wait_for(2000, [&] { return sock.lines().size() > before; }));
        CHECK(sock.lines().back() == "dim deny\n");
        link.request_async("dim allow");
        CHECK(wait_for(2000, [&] { return sock.lines().back() == "dim allow\n"; }));
        CHECK_EQ(link.refused(), 0);
        CHECK(wait_for(1000, [&] { return link.sent() >= 2; }));
    }
    {
        auto fio = std::make_unique<FakeIo>();
        FakeIo *f = fio.get();
        f->files["/b"] = "v=1\npid=5\nbattery=0\nbacklight=none\nrequests=hello,caps,toast,beep\n"; /* a board that answers no dim */
        LauncherLink link(sock.path, std::move(fio), "/s", "/b");
        const size_t before = sock.lines().size();
        link.request_async("dim deny");
        link.request_async("volume set 40");
        link.request_async("hello"); /* other verbs always go */
        CHECK(wait_for(2000, [&] { return sock.lines().size() > before; }));
        sleep_ms(300);
        const std::vector<std::string> all = sock.lines();
        CHECK_EQ(all.size(), before + 1);
        CHECK(all.back() == "hello\n");
        CHECK_EQ(link.refused(), 2);
    }
    {
        /* no files at all (an older launcher, standalone runs): nothing for dim */
        LauncherLink link(sock.path, std::make_unique<FakeIo>(), "/s", "/b");
        CHECK(!link.supports("dim"));
        const size_t before = sock.lines().size();
        link.request_async("dim allow");
        sleep_ms(250);
        CHECK_EQ(sock.lines().size(), before);
        CHECK_EQ(link.refused(), 1);
    }
}

/* ------------------------------------------------------------------------------------------------ the real world */

struct EnvGuard {
    std::vector<std::pair<std::string, std::string>> saved;
    std::vector<std::string> unset;
    void set(const char *k, const std::string &v) {
        const char *o = std::getenv(k);
        if (o) saved.emplace_back(k, o);
        else unset.emplace_back(k);
        ::setenv(k, v.c_str(), 1);
    }
    ~EnvGuard() {
        for (const auto &kv : saved) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
        for (const std::string &k : unset) ::unsetenv(k.c_str());
    }
};

/* The real constructor with no sound device: no sound server (PULSE_SERVER names nothing), no launcher, an ALSA default that
 * cannot be opened on the build box. The player must say so and keep the audio: no crash, has_audio, an error. (On a box with
 * a working default output the stream simply plays: both are accepted.) */
void real_world_tests(const Media &m, const std::string &dir) {
    if (m.tone.empty()) {
        SKIP("playaudio real world: ffmpeg could not make the tone fixture");
        return;
    }
    EnvGuard env;
    env.set("PULSE_SERVER", "unix:" + dir + "/no-such-pulse-socket");
    env.set("XDG_RUNTIME_DIR", dir);
    env.set("VHS_OUTPUT_LEVELS", dir + "/real-levels");
    {
        AudioPlayer p("");
        CHECK(p.open(m.tone, 0, true));
        AudioClock c = p.clock();
        CHECK(c.open && c.has_audio);
        CHECK(wait_clock(p, 4000, [](const AudioClock &x) { return !x.error.empty() || x.heard_ms > 100 || x.ended; }));
        c = p.clock();
        CHECK(c.has_audio);
        if (!c.error.empty()) {
            CHECK(!c.playing);
            CHECK(c.error.find("cannot play") != std::string::npos);
            std::fprintf(stderr, "playaudio real world: no output here, as expected (%s)\n", c.error.c_str());
        } else {
            std::fprintf(stderr, "playaudio real world: the box has a working default output; the clip played\n");
        }
        (void)p.volume_percent(); /* no crash whatever the world */
        p.volume_step(-1);
        (void)p.poll_key();
        (void)p.output_rows();
        p.close();
        CHECK(!p.clock().open);
        p.shutdown();
        p.shutdown(); /* idempotent */
        CHECK(!p.open(m.tone, 0, false)); /* after shutdown: nothing */
    }
}

}  // namespace

void test_play_audio() {
    const std::string base = make_temp_dir("playaudio");
    if (base.empty()) {
        CHECK(false);
        return;
    }
    volume_model_tests();
    output_tests();
    media_key_tests(base);
    launcher_tests(base);

    std::string why;
    if (!real_ffmpeg(&why)) {
        std::fprintf(stderr, "playaudio: SKIPPED the engine, decoder and player cases LOUDLY: no FFmpeg libraries on this box (%s)\n",
                     why.c_str());
        SKIP("playaudio: no FFmpeg libraries");
        rm_tree(base);
        return;
    }
    std::fprintf(stderr, "playaudio: FFmpeg %s (libavformat %d)\n", real_ffmpeg()->name.c_str(), real_ffmpeg()->lavf_major);
    const Media m = make_media(base);
    if (!m.ffmpeg) {
        std::fprintf(stderr, "playaudio: ffmpeg is MISSING on this box: the MKV cases are NOT run (apt install ffmpeg)\n");
        SKIP("playaudio: no ffmpeg for the fixtures");
    }
    engine_tests(base); /* WAV files made in-process: no ffmpeg needed */
    if (m.ffmpeg) {
        decoder_tests(m);
        player_tests(m);
        volume_player_tests(m, base);
        fake_output_tests(m);
        real_world_tests(m, base);
    }
    rm_tree(base);
}
