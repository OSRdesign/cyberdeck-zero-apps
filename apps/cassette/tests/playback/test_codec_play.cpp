/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette playback tests (phase C): the Player on FLAC and Ogg Opus takes (the fake decoders of tests/fake_decoders.hpp
 * and the test encoders of tests/fixture_codecs.hpp). Each compressed take has a twin WAV file holding exactly the PCM
 * its decoder delivers; the same scripts run on both Players and everything must be identical, call by call: the state,
 * the position heard, the events, and every sample the device received. So play, pause and resume, seek, the 5x scan
 * both ways (and a change of direction), REW stopping at the start, FWD at the end, the 5 s jump and the end of a take
 * behave for FLAC and Opus exactly as for WAV. Also: a take whose library is missing fails to open with kCodecMissing,
 * and the cost of a period per format (fakes: the Player's and the reader's own work).
 */
#include "check.hpp"
#include "player.hpp"
#include "support.hpp"

#include "../fake_decoders.hpp"
#include "../fixture_codecs.hpp"

#include <chrono>
#include <functional>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

struct Libs {
    FlacDecLib flac = fakedec::make_flac_dec_lib();
    OpusDecLib opus = fakedec::make_opus_dec_lib();
    DecoderLibs d;
    Libs() {
        d.flac = &flac;
        d.opus = &opus;
    }
};
Libs &libs() {
    static Libs l;
    return l;
}

std::vector<unsigned char> le_bytes(const std::vector<int32_t> &s, unsigned bits) {
    std::vector<unsigned char> b;
    for (int32_t v : s)
        for (unsigned k = 0; k < bits / 8u; ++k) b.push_back(static_cast<unsigned char>((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF));
    return b;
}

void put_file(const std::string &p, const std::vector<unsigned char> &b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct Twin {
    std::string name, comp, wav;
    AudioFormat fmt;
    uint64_t frames = 0;
};

Twin flac_twin(const TempDir &d, const char *name, const AudioFormat &f, uint64_t frames) {
    Twin t;
    t.name = name;
    t.fmt = f;
    t.frames = frames;
    const std::vector<int32_t> s = ramp_samples(frames, f.channels, f.bits);
    t.comp = d.file(std::string(name) + ".flac");
    t.wav = d.file(std::string(name) + ".wav");
    MemoryByteSink sink;
    fx::VerbatimFlac e(f, 4096);
    e.start(sink, {});
    const std::vector<unsigned char> pcm = le_bytes(s, f.bits);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    put_file(t.comp, sink.data);
    write_wav(t.wav, f, s);
    return t;
}

Twin opus_twin(const TempDir &d, const char *name, const AudioFormat &f, uint64_t frames, unsigned page_packets) {
    Twin t;
    t.name = name;
    t.comp = d.file(std::string(name) + ".opus");
    t.wav = d.file(std::string(name) + ".wav");
    MemoryByteSink sink;
    fx::PlainOggOpus e(f, 60, page_packets, 312);
    e.start(sink, {});
    const std::vector<unsigned char> pcm(static_cast<size_t>(frames) * f.channels * 2, 0);
    e.write(pcm.data(), static_cast<size_t>(frames));
    e.finish();
    put_file(t.comp, sink.data);
    const std::vector<int16_t> dec = fakedec::expected_opus_pcm(posix_file_ops(), t.comp);
    t.fmt = afmt(oggopus::decode_rate(f.rate), f.channels, 16);
    t.frames = dec.size() / f.channels;
    write_wav(t.wav, t.fmt, std::vector<int32_t>(dec.begin(), dec.end()));
    return t;
}

std::vector<PlayerEvent> drain_events(Player &p) {
    std::vector<PlayerEvent> v;
    PlayerEvent e;
    while (p.poll_event(e)) v.push_back(e);
    return v;
}

/* Two players, one per file; every operation goes to both and the results are compared. */
struct Pair {
    FakeSink s1, s2;
    std::unique_ptr<Player> a, b;
    int mismatches = 0;
    std::string where;
    Pair(const Twin &t, long latency) {
        s1.latency = s2.latency = latency;
        a.reset(new Player(posix_file_ops(), s1, Player::Mode::Manual, &libs().d));
        b.reset(new Player(posix_file_ops(), s2, Player::Mode::Manual, &libs().d));
        CHECK_EQ(a->open(t.wav), 0);
        CHECK_EQ(b->open(t.comp), 0);
        compare("open");
    }
    void compare(const std::string &what) {
        const bool same = a->state() == b->state() && a->position() == b->position() && a->length() == b->length() &&
                          drain_events(*a) == drain_events(*b) && a->format().rate == b->format().rate &&
                          a->format().channels == b->format().channels && s1.heard_frames() == s2.heard_frames();
        if (!same) {
            if (!mismatches) where = what;
            ++mismatches;
        }
    }
    void op(const std::string &what, const std::function<int(Player &)> &f) {
        const int ra = f(*a), rb = f(*b);
        if (ra != rb) {
            if (!mismatches) where = what + " (result)";
            ++mismatches;
        }
        compare(what);
    }
    void steps(int n) {
        for (int i = 0; i < n; ++i) {
            const bool x = a->step(), y = b->step();
            if (x != y) {
                if (!mismatches) where = "step result";
                ++mismatches;
            }
            compare("step");
        }
    }
    void to_end(int max = 4000) {
        for (int i = 0; i < max && a->state() != PlayerState::Stopped; ++i) steps(1);
    }
    bool heard_equal() const { return s1.heard() == s2.heard(); }
};

void scripts(const Twin &t) {
    const uint64_t L = t.frames;
    const unsigned rate = t.fmt.rate;
    for (long latency : {0L, 3000L}) {
        std::vector<std::pair<std::string, std::function<void(Pair &)>>> sc = {
            {"play to the end",
             [](Pair &p) {
                 p.op("play", [](Player &x) { return x.play(); });
                 p.to_end();
             }},
            {"pause and resume",
             [](Pair &p) {
                 p.op("play", [](Player &x) { return x.play(); });
                 p.steps(3);
                 p.op("pause", [](Player &x) { return x.pause(); });
                 p.steps(2);
                 p.op("resume", [](Player &x) { return x.resume(); });
                 p.steps(4);
                 p.op("pause", [](Player &x) { return x.pause(); });
                 p.op("play", [](Player &x) { return x.play(); });
                 p.to_end();
             }},
            {"scan forward to the end",
             [](Pair &p) {
                 p.op("play", [](Player &x) { return x.play(); });
                 p.steps(2);
                 p.op("scan fwd", [](Player &x) { return x.scan(Direction::Forward); });
                 p.to_end();
                 p.op("end_scan after the end", [](Player &x) { return x.end_scan(); });
             }},
            {"scan reverse to the start",
             [L](Pair &p) {
                 p.op("seek", [L](Player &x) { return x.seek(L * 2 / 3); });
                 p.op("play", [](Player &x) { return x.play(); });
                 p.steps(2);
                 p.op("scan rev", [](Player &x) { return x.scan(Direction::Reverse); });
                 p.to_end();
                 p.op("play from 0", [](Player &x) { return x.play(); });
                 p.steps(3);
             }},
            {"REW at the start, FWD at the end",
             [L](Pair &p) {
                 p.op("scan rev at 0", [](Player &x) { return x.scan(Direction::Reverse); });
                 p.steps(2);
                 p.op("seek end", [L](Player &x) { return x.seek(L); });
                 p.op("scan fwd at end", [](Player &x) { return x.scan(Direction::Forward); });
                 p.steps(2);
             }},
            {"jumps",
             [L](Pair &p) {
                 p.op("play", [](Player &x) { return x.play(); });
                 p.steps(1);
                 p.op("jump fwd", [](Player &x) { return x.jump(Direction::Forward); });
                 p.steps(2);
                 for (int i = 0; i < 3; ++i) p.op("jump back", [](Player &x) { return x.jump(Direction::Reverse); });
                 p.steps(2);
                 p.op("seek near the end", [L](Player &x) { return x.seek(L - 3000); });
                 p.op("jump fwd past the end", [](Player &x) { return x.jump(Direction::Forward); });
                 p.to_end();
             }},
            {"scan both ways, release, seek while paused",
             [L, rate](Pair &p) {
                 p.op("seek", [L](Player &x) { return x.seek(L / 3); });
                 p.op("play", [](Player &x) { return x.play(); });
                 p.steps(1);
                 p.op("scan fwd", [](Player &x) { return x.scan(Direction::Forward); });
                 p.steps(3);
                 p.op("scan rev", [](Player &x) { return x.scan(Direction::Reverse); });
                 p.steps(5);
                 p.op("end scan", [](Player &x) { return x.end_scan(); });
                 p.steps(2);
                 p.op("pause", [](Player &x) { return x.pause(); });
                 for (uint64_t k = 1; k < 6; ++k) {
                     const uint64_t at = (k * 7919 * rate / 10) % L;
                     p.op("seek paused", [at](Player &x) { return x.seek(at); });
                 }
                 p.op("resume", [](Player &x) { return x.resume(); });
                 p.steps(3);
                 p.op("stop", [](Player &x) { return x.stop(); });
                 p.op("play after stop", [](Player &x) { return x.play(); });
                 p.steps(2);
             }},
        };
        for (auto &s : sc) {
            Pair p(t, latency);
            s.second(p);
            CHECK_EQ(p.mismatches, 0);
            CHECK(p.heard_equal());
            if (p.mismatches || !p.heard_equal())
                std::fprintf(stderr, "  %s, latency %ld, \"%s\": differs from WAV (first at %s)\n", t.name.c_str(), latency,
                             s.first.c_str(), p.where.c_str());
        }
    }
}

/* The cost of one period with the fakes: the Player plus the reader (no codec work to speak of). */
void cost(const Twin &t) {
    for (int which = 0; which < 2; ++which) {
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual, &libs().d);
        CHECK_EQ(p.open(which ? t.comp : t.wav), 0);
        p.seek(t.frames / 2);
        p.scan(Direction::Forward);
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        while (p.state() == PlayerState::Scanning && n < 2000) {
            p.step();
            ++n;
        }
        const double fwd = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / std::max(n, 1);
        p.seek(t.frames / 2);
        p.scan(Direction::Reverse);
        t0 = std::chrono::steady_clock::now();
        int m = 0;
        while (p.state() == PlayerState::Scanning && m < 2000) {
            p.step();
            ++m;
        }
        const double rev = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / std::max(m, 1);
        std::printf("  %s %s: a 5x scan period costs %.0f us forward, %.0f us reverse (fakes, this PC; a period is %.1f ms)\n",
                    t.name.c_str(), which ? "decoded" : "WAV twin", fwd, rev, 1024.0 * 1000.0 / t.fmt.rate);
    }
}

}  // namespace

void test_codec_play() {
    TempDir d;
    const Twin twins[] = {
        flac_twin(d, "FLAC 48k mono", afmt(48000, 1, 16), 48000 * 12 + 77),
        flac_twin(d, "FLAC 44.1k stereo 24-bit", afmt(44100, 2, 24), 44100 * 11),
        opus_twin(d, "Opus 48k stereo", afmt(48000, 2, 16), 48000 * 12, 50),
        opus_twin(d, "Opus 16k mono (plays at 16 kHz)", afmt(16000, 1, 16), 16000 * 12, 7),
    };
    for (const Twin &t : twins) {
        scripts(t);
        std::printf("  %s: 7 scripts x 2 device latencies identical to its WAV twin\n", t.name.c_str());
    }
    cost(twins[0]);
    cost(twins[2]);
    /* a take whose library is missing */
    DecoderLibs none;
    FakeSink sink;
    Player p(posix_file_ops(), sink, Player::Mode::Manual, &none);
    CHECK_EQ(p.open(twins[0].comp), kCodecMissing);
    CHECK_EQ(p.play(), -ENOENT);
    CHECK_EQ(sink.opens(), 0u);
    CHECK_EQ(p.open(twins[0].wav), 0); /* WAV needs nothing */
    CHECK_EQ(p.play(), 0);
}
