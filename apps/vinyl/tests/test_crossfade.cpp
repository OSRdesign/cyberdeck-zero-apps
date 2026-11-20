/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests (P2d): the crossfade's arithmetic (lengths, equal-power curves, the mix) and the engine with two decoders
 * on WAV files and a fake output: the fade's frames and levels, the format / skip rules (no fade: the gapless change), a
 * track shorter than the fade, a queue of four tracks with each Transition (a fade on every change, the same album or
 * not), a seek, a pause and a skip during a fade; the equalizer through the engine (a boost raises its band and leaves
 * the others at their level, no clipping at the strongest curve, the bypass bit-exact); both through the FFmpeg tier when
 * the fixture is there.
 */
#include "check.hpp"

#include "decode/decode_libs.hpp"
#include "dsp/crossfade.hpp"
#include "dsp/eq.hpp"
#include "engine/engine.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sys/stat.h>
#include <thread>

using namespace vinyl;

namespace {

constexpr size_t kD = 221; /* the limiter's 5 ms at 44.1 kHz: a float stream starts with it and ends with its tail */

/* a 16-bit stereo WAV: left `l`, right `r` on every frame */
std::string dc_wav(uint32_t rate, int frames, int16_t l, int16_t r) {
    std::string d;
    d.reserve(static_cast<size_t>(frames) * 4);
    for (int i = 0; i < frames; ++i) {
        put_le16(d, static_cast<uint16_t>(l));
        put_le16(d, static_cast<uint16_t>(r));
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

/* a sine (both channels), amplitude amp of full scale; square: a square wave of that frequency instead */
std::string tone_wav(uint32_t rate, int frames, double freq, double amp, bool square = false) {
    std::string d;
    d.reserve(static_cast<size_t>(frames) * 4);
    for (int i = 0; i < frames; ++i) {
        double v = std::sin(2 * 3.14159265358979 * freq * i / rate);
        if (square) v = v >= 0 ? 1 : -1;
        const int s = static_cast<int>(std::lround(std::max(-1.0, std::min(1.0, v * amp)) * 32767));
        put_le16(d, static_cast<uint16_t>(s));
        put_le16(d, static_cast<uint16_t>(s));
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

struct Record {
    std::mutex mu;
    std::vector<int16_t> samples;
    int opens = 0, drains = 0, drops = 0;
    double speed = 0; /* 0: instant; else paced at speed x real time (`lead` s of audio ahead at most) */
    double lead = 0.05;
    /* called on the engine thread after every write (not under any lock of the engine's): the queue test's controller,
     * which reads the events and names the next track before the engine's next period, as the app does in its tick */
    std::function<void()> after_write;
};

class FakeSink final : public PlaybackSink {
public:
    explicit FakeSink(Record &r) : r_(r) {}
    int open(const AudioFormat &src, SinkFormat &g) override {
        std::lock_guard<std::mutex> l(r_.mu);
        ++r_.opens;
        g.rate = src.rate;
        g.channels = src.channels;
        g.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        rate_ = src.rate;
        start_ = std::chrono::steady_clock::now();
        written_ = 0;
        return 0;
    }
    long write(const void *f, size_t n) override {
        {
            std::lock_guard<std::mutex> l(r_.mu);
            const int16_t *s = static_cast<const int16_t *>(f);
            r_.samples.insert(r_.samples.end(), s, s + n * 2);
        }
        if (r_.speed > 0)
            while (static_cast<double>(written_) > heard() + rate_ * r_.lead) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        written_ += n;
        if (r_.after_write) r_.after_write();
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override {
        if (r_.speed > 0) return written_ > heard() ? static_cast<long>(written_ - static_cast<uint64_t>(heard())) : 0;
        return 0;
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
    void close() override {}

private:
    double heard() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() * rate_ * r_.speed; }
    Record &r_;
    uint32_t rate_ = 44100;
    std::chrono::steady_clock::time_point start_;
    uint64_t written_ = 0;
};

class FakeProvider final : public SinkProvider {
public:
    explicit FakeProvider(Record &r) : r_(r) {}
    int open(const OutputTarget &, OpenedSink &o, std::string &) override {
        o.sink = std::make_unique<FakeSink>(r_);
        o.name = o.device = "default";
        o.label = "Fake output";
        return 0;
    }
    bool still_there(const OpenedSink &) override { return true; }

private:
    Record &r_;
};

/* waits for the end of the queue; `seen` gets the events */
bool wait_end(Engine &e, std::vector<EngineEvent> &seen, int ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        EngineEvent ev;
        while (e.poll_event(ev)) {
            seen.push_back(ev);
            if (ev.kind == EngineEvent::Kind::QueueEnd) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

template <typename F> bool wait_until(F f, int ms = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool near(int a, double b, double tol = 2.5) { return std::fabs(a - b) <= tol; }

/* ---------------------------------------------------------------------------------------------- the arithmetic */

void test_math() {
    /* the crossfades' seconds (the Transition's 5, 10, 20 sec; owner, 2026-11-20) */
    CHECK_EQ(crossfade_word(0), std::string("Off"));
    CHECK_EQ(crossfade_word(5), std::string("5 s"));
    CHECK_EQ(crossfade_word(20), std::string("20 s"));
    CHECK_EQ(crossfade_word(6), std::string("Off")); /* the draft's 2 ... 8 s are no choice any more */
    /* Now playing's transition chip (owner, 2026-11-20): each of the five choices; Normal none; a running fade shows that
     * fade (its own seconds) whatever the setting is now */
    CHECK_EQ(transition_chip_text(Transition::Normal), std::string(""));
    CHECK_EQ(transition_chip_text(Transition::Gapless), std::string("Gapless"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade5), std::string("Crossfade 5 s"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade10), std::string("Crossfade 10 s"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade20), std::string("Crossfade 20 s"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade10, true, 10), std::string("Crossfade 10 s"));
    CHECK_EQ(transition_chip_text(Transition::Normal, true, 20), std::string("Crossfade 20 s")); /* changed mid-fade */
    CHECK_EQ(transition_chip_text(Transition::Gapless, true, 5), std::string("Crossfade 5 s"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade20, true, 0), std::string("Crossfade 20 s")); /* unknown: the setting's */
    CHECK_EQ(transition_chip_text(Transition::Normal, true, 0), std::string("Crossfade"));
    CHECK_EQ(transition_chip_text(Transition::Crossfade5, false, 20), std::string("Crossfade 5 s")); /* no fade: the setting */
    CHECK_EQ(crossfade_valid(10), 10);
    CHECK_EQ(crossfade_valid(2), 0);
    CHECK_EQ(crossfade_valid(-2), 0);
    /* lengths: the setting; half of either track at most; under half a second none */
    CHECK_EQ(crossfade_frames(0, 44100, 0, 0), static_cast<uint64_t>(0));
    CHECK_EQ(crossfade_frames(5, 44100, 44100 * 300, 44100 * 200), static_cast<uint64_t>(5 * 44100));
    CHECK_EQ(crossfade_frames(20, 44100, 44100 * 300, 44100 * 200), static_cast<uint64_t>(20 * 44100));
    CHECK_EQ(crossfade_frames(20, 44100, 44100 * 30, 44100 * 200), static_cast<uint64_t>(15 * 44100)); /* A of 30 s: 15 s */
    CHECK_EQ(crossfade_frames(5, 44100, 44100 * 4, 0), static_cast<uint64_t>(2 * 44100));      /* A of 4 s: 2 s */
    CHECK_EQ(crossfade_frames(5, 44100, 44100 * 300, 44100 * 3), static_cast<uint64_t>(66150)); /* B of 3 s: 1.5 s */
    CHECK_EQ(crossfade_frames(10, 48000, 0, 0), static_cast<uint64_t>(480000));                /* lengths unknown: no limit */
    CHECK_EQ(crossfade_frames(5, 44100, 44100, 0), static_cast<uint64_t>(22050));              /* 1 s track: 0.5 s, the least */
    CHECK_EQ(crossfade_frames(5, 44100, 44099, 0), static_cast<uint64_t>(0));                  /* just under: none */
    CHECK_EQ(crossfade_frames(5, 0, 44100, 0), static_cast<uint64_t>(0));
    /* the controller's rule: every change into another track when the setting fades, the same album or not; never repeat
     * one (the same position), never into nothing, never where the sleep timer stops */
    CHECK(crossfade_named(5, 0, 1, false));
    CHECK(crossfade_named(20, 7, 0, false)); /* repeat all: the last into the first */
    CHECK(!crossfade_named(0, 0, 1, false));  /* Normal, Gapless */
    CHECK(!crossfade_named(5, 3, 3, false));  /* repeat one */
    CHECK(!crossfade_named(5, 3, -1, false)); /* the queue's end */
    CHECK(!crossfade_named(5, 3, 4, true));   /* the sleep timer stops here */
    CHECK(!crossfade_named(5, -1, 0, false));
    /* equal power: cos^2 + sin^2 = 1 everywhere, the ends near 1 and 0, the middle at 0.7071 */
    bool power = true;
    for (uint64_t i = 0; i < 1000; i += 37) {
        double o = 0, n = 0;
        crossfade_gains(i, 1000, o, n);
        power = power && std::fabs(o * o + n * n - 1.0) < 1e-12 && o >= 0 && n >= 0;
    }
    CHECK(power);
    double o = 0, n = 0;
    crossfade_gains(0, 88200, o, n);
    CHECK(o > 0.99999 && n < 0.0001);
    crossfade_gains(44100, 88200, o, n);
    CHECK_NEAR(o, 0.70710, 1e-4);
    CHECK_NEAR(n, 0.70711, 1e-4);
    crossfade_gains(88200, 88200, o, n); /* past the end: the incoming track alone */
    CHECK_EQ(o, 0.0);
    CHECK_EQ(n, 1.0);
    /* the mix by rotation equals the direct cos / sin, block after block, and past the end the incoming track alone */
    {
        const uint64_t len = 5000;
        std::vector<float> a(2 * 2048), b(2 * 2048), d(2 * 2048);
        double worst = 0;
        for (uint64_t pos = 0; pos < len + 2048; pos += 2048) {
            for (size_t f = 0; f < 2048; ++f) {
                a[2 * f] = 0.8f;
                a[2 * f + 1] = -0.4f;
                b[2 * f] = 0.3f;
                b[2 * f + 1] = 0.6f;
            }
            crossfade_mix(a.data(), b.data(), d.data(), 2048, pos, len);
            for (size_t f = 0; f < 2048; ++f) {
                double go = 0, gi = 0;
                crossfade_gains(pos + f, len, go, gi);
                worst = std::max(worst, std::fabs(d[2 * f] - (0.8 * go + 0.3 * gi)));
                worst = std::max(worst, std::fabs(d[2 * f + 1] - (-0.4 * go + 0.6 * gi)));
            }
        }
        CHECK(worst < 1e-5);
        /* in place (dst = one of the inputs) */
        std::fill(a.begin(), a.end(), 1.0f);
        std::fill(b.begin(), b.end(), 0.0f);
        crossfade_mix(a.data(), b.data(), a.data(), 2048, 0, 4096);
        CHECK(a[0] > 0.9999f);
        CHECK_NEAR(a[2 * 2047], std::cos(1.5707963267948966 * 2047.5 / 4096), 1e-5); /* half way: cos 45 degrees */
    }
}

/* ---------------------------------------------------------------------------------------------- the engine */

struct Out {
    std::vector<int16_t> s;
    int opens = 0;
    bool ended = false;
    std::vector<EngineEvent> seen;
};

Out play_two(const std::string &a, const std::string &b, int fade_s, bool may_fade) {
    Record r;
    FakeProvider p(r);
    Engine e(DecodeLibs(), p);
    e.set_crossfade_s(fade_s);
    e.load(0, a, 0, true);
    e.set_next(1, b, may_fade);
    Out o;
    o.ended = wait_end(e, o.seen);
    std::lock_guard<std::mutex> l(r.mu);
    o.s = r.samples;
    o.opens = r.opens;
    return o;
}

void test_engine_fades(const std::string &dir) {
    write_file(dir + "/a5.wav", dc_wav(44100, 44100 * 5, 8000, 4000));
    write_file(dir + "/b5.wav", dc_wav(44100, 44100 * 5, 2000, 1000));
    write_file(dir + "/b15.wav", dc_wav(44100, 66150, 2000, 1000)); /* 1.5 s */
    write_file(dir + "/a08.wav", dc_wav(44100, 35280, 8000, 4000)); /* 0.8 s */
    write_file(dir + "/b48.wav", dc_wav(48000, 48000 * 2, 2000, 1000));

    /* 5 s asked between two 5 s tracks: 2.5 s (half of either), one stream, 5 + 5 - 2.5 s (+ the limiter's 5 ms), A alone,
     * then A cos + B sin, then B alone; the incoming track is Advanced (named next) at the mix's start and Started after */
    {
        const Out o = play_two(dir + "/a5.wav", dir + "/b5.wav", 5, true);
        CHECK(o.ended);
        CHECK_EQ(o.opens, 1);
        const size_t L = 110250, start = 220500 - L;
        const size_t frames = 220500 + 220500 - L + kD;
        CHECK_EQ(o.s.size(), frames * 2);
        if (o.s.size() == frames * 2) {
            auto at = [&](size_t k, int ch) { return static_cast<int>(o.s[2 * (k + kD) + static_cast<size_t>(ch)]); };
            CHECK_EQ(o.s[0], 0); /* the limiter's delay */
            CHECK(near(at(100000, 0), 8000) && near(at(100000, 1), 4000));
            CHECK(near(at(start - 1, 0), 8000));
            double go = 0, gi = 0;
            crossfade_gains(L / 2, L, go, gi);
            CHECK(near(at(start + L / 2, 0), 8000 * go + 2000 * gi) && near(at(start + L / 2, 1), 4000 * go + 1000 * gi));
            crossfade_gains(10, L, go, gi);
            CHECK(near(at(start + 10, 0), 8000 * go + 2000 * gi));
            CHECK(near(at(220500 + 5, 0), 2000) && near(at(220500 + 5, 1), 1000)); /* the fade over: B alone */
            CHECK(near(at(frames - kD - 1, 0), 2000));
            /* equal power on two equal-phase levels: A cos + B sin never under the quieter one nor over sqrt(A^2 + B^2)
             * (8246 here: the reason a fade goes through the limiter, whose ceiling is 29204) */
            bool bounded = true;
            for (size_t k = start; k < 220500; k += 441) bounded = bounded && at(k, 0) <= 8247 && at(k, 0) >= 1999;
            CHECK(bounded);
        }
        bool adv = false, started = false;
        for (const EngineEvent &ev : o.seen) {
            if (ev.kind == EngineEvent::Kind::Advanced && ev.pos == 1) adv = true;
            if (ev.kind == EngineEvent::Kind::Started && ev.pos == 1) started = adv;
        }
        CHECK(adv);
        CHECK(started);
    }
    /* named without may_fade (repeat one, the sleep timer's stop): the gapless change, back to back (the float path stays in) */
    {
        const Out o = play_two(dir + "/a5.wav", dir + "/b5.wav", 5, false);
        CHECK(o.ended);
        CHECK_EQ(o.opens, 1);
        CHECK_EQ(o.s.size(), (441000 + kD) * 2);
        if (o.s.size() == (441000 + kD) * 2) {
            CHECK_EQ(o.s[2 * (kD + 220499)], 8000);
            CHECK_EQ(o.s[2 * (kD + 220500)], 2000);
        }
    }
    /* crossfade Off: the integer path of P1, bit-exact, back to back, no delay (gapless untouched) */
    {
        const Out o = play_two(dir + "/a5.wav", dir + "/b5.wav", 0, true);
        CHECK(o.ended);
        CHECK_EQ(o.s.size(), static_cast<size_t>(441000 * 2));
        bool exact = o.s.size() == static_cast<size_t>(441000 * 2);
        for (size_t f = 0; exact && f < 441000; f += 97)
            exact = o.s[2 * f] == (f < 220500 ? 8000 : 2000) && o.s[2 * f + 1] == (f < 220500 ? 4000 : 1000);
        CHECK(exact);
    }
    /* the incoming track shorter than twice the fade: 5 s asked, B of 1.5 s fades for 0.75 s (half of it) */
    {
        const Out o = play_two(dir + "/a5.wav", dir + "/b15.wav", 5, true);
        CHECK(o.ended);
        const size_t frames = 220500 + 66150 - 33075 + kD;
        CHECK_EQ(o.s.size(), frames * 2);
        if (o.s.size() == frames * 2) {
            CHECK(near(o.s[2 * (kD + 220500 - 33075 - 1)], 8000));      /* A alone up to the fade */
            CHECK(near(o.s[2 * (kD + 220500 + 100)], 2000));            /* B alone after it */
        }
    }
    /* the outgoing track shorter than a second: 0.8 s gives 0.4 s, under half a second: no fade (back to back) */
    {
        const Out o = play_two(dir + "/a08.wav", dir + "/b5.wav", 20, true);
        CHECK(o.ended);
        CHECK_EQ(o.s.size(), (35280 + 220500 + kD) * 2);
    }
    /* another format (48 kHz): no fade (D29), the stream drains and opens again */
    {
        const Out o = play_two(dir + "/a5.wav", dir + "/b48.wav", 5, true);
        CHECK(o.ended);
        CHECK_EQ(o.opens, 2);
        CHECK_EQ(o.s.size(), (220500 + kD + 96000 + 240) * 2); /* each stream with its limiter's delay (240 at 48 kHz) */
    }
}

/* A queue of four tracks played to its end with each Transition (owner, 2026-11-20), the controller emulated as the app
 * does it (vinyl_app name_next): on Advanced the track after the decoder's is named, with crossfade_named's may_fade; on
 * Started it is named again. The board report of the 0.7.0 draft ("one transition fades, the next one does not") was the
 * draft's rule D29 (never inside an album: two albums queued in order give one change between albums); every change
 * fades now, the same album or not, and this checks all three changes of the queue, not only the first. */
struct QueueOut {
    std::vector<int16_t> s;
    int opens = 0;
    bool ended = false;
    int advanced = 0, started = 0;
};

QueueOut play_queue(const std::vector<std::string> &q, Transition t) {
    Record r;
    FakeProvider p(r);
    Engine e(DecodeLibs(), p);
    e.set_gapless(transition_gapless(t));
    e.set_crossfade_s(transition_crossfade_s(t));
    QueueOut o;
    int dec = -1;
    auto name = [&](int pos) {
        const int n = pos >= 0 && pos + 1 < static_cast<int>(q.size()) ? pos + 1 : -1;
        e.set_next(n, n >= 0 ? q[static_cast<size_t>(n)] : std::string(), crossfade_named(transition_crossfade_s(t), pos, n, false));
    };
    /* on the engine thread after each period written: the next command is handled before the next period */
    r.after_write = [&] {
        EngineEvent ev;
        while (e.poll_event(ev)) {
            if (ev.kind == EngineEvent::Kind::Advanced) {
                dec = ev.pos;
                ++o.advanced;
                name(ev.pos);
            } else if (ev.kind == EngineEvent::Kind::Started) {
                ++o.started;
                name(dec);
            }
        }
    };
    e.load(0, q[0], 0, true);
    /* the queue's end pauses the engine (QueueEnd, after which nothing is written) */
    o.ended = wait_until([&] { return e.snapshot().state == PlayState::Paused; }, 20000);
    EngineEvent ev;
    while (e.poll_event(ev)) {
        if (ev.kind == EngineEvent::Kind::Advanced) ++o.advanced;
        if (ev.kind == EngineEvent::Kind::Started) ++o.started;
    }
    std::lock_guard<std::mutex> l(r.mu);
    o.s = r.samples;
    o.opens = r.opens;
    return o;
}

void test_engine_queue(const std::string &dir) {
    /* 45 s each at 8 kHz (small files, long enough for 20 s: half of 45 s is 22.5 s); A, B in one folder (one album), C, D
     * in another */
    constexpr uint32_t kRate = 8000;
    constexpr size_t T = 45 * kRate;
    constexpr size_t kD8 = 40; /* the limiter's 5 ms at 8 kHz */
    const int lv[4] = {8000, 2000, 6000, 1000};
    ::mkdir((dir + "/album1").c_str(), 0755);
    ::mkdir((dir + "/album2").c_str(), 0755);
    const std::vector<std::string> q = {dir + "/album1/a.wav", dir + "/album1/b.wav", dir + "/album2/c.wav", dir + "/album2/d.wav"};
    for (int i = 0; i < 4; ++i)
        write_file(q[static_cast<size_t>(i)], dc_wav(kRate, static_cast<int>(T), static_cast<int16_t>(lv[i]), static_cast<int16_t>(lv[i] / 2)));
    for (Transition t : kTransitions) {
        const QueueOut o = play_queue(q, t);
        const int fade_s = transition_crossfade_s(t);
        const size_t L = fade_s ? static_cast<size_t>(fade_s) * kRate : 0;
        const size_t frames = fade_s ? 4 * T - 3 * L + kD8 : 4 * T;
        std::fprintf(stderr, "  queue of 4 with Transition %s: %zu frames (expected %zu), %d stream(s), %d advanced, %d started\n",
                     transition_word(t), o.s.size() / 2, frames, o.opens, o.advanced, o.started);
        CHECK(o.ended);
        CHECK_EQ(o.advanced, 4);
        CHECK_EQ(o.started, 4);
        CHECK_EQ(o.opens, t == Transition::Normal ? 4 : 1); /* Normal: every change drains and opens again */
        CHECK_EQ(o.s.size(), frames * 2);
        if (o.s.size() != frames * 2) continue;
        if (!fade_s) { /* back to back, bit-exact (the integer path) */
            bool exact = true;
            for (size_t i = 1; i < 4; ++i)
                exact = exact && o.s[2 * (i * T - 1)] == lv[i - 1] && o.s[2 * (i * T)] == lv[i] && o.s[2 * (i * T) + 1] == lv[i] / 2;
            CHECK(exact);
            continue;
        }
        /* every one of the three changes is a fade of the full length (the same album A -> B and C -> D, the albums' change
         * B -> C): the outgoing track alone up to it, cos / sin in its middle, the incoming one alone after it */
        auto at = [&](size_t k) { return static_cast<int>(o.s[2 * (k + kD8)]); };
        double go = 0, gi = 0;
        crossfade_gains(L / 2, L, go, gi);
        CHECK(near(at(T - L - 1), lv[0]));
        for (size_t i = 1; i < 4; ++i) {
            const size_t s = i * (T - L); /* where change i starts */
            const bool mid = near(at(s + L / 2), lv[i - 1] * go + lv[i] * gi);
            const bool before = near(at(s - 1), lv[i - 1]);
            const bool after = near(at(s + L + 5), lv[i]);
            if (!mid || !before || !after)
                std::fprintf(stderr, "  change %zu of %s: before %d, middle %d (want %.0f), after %d\n", i, transition_word(t), at(s - 1),
                             at(s + L / 2), lv[i - 1] * go + lv[i] * gi, at(s + L + 5));
            CHECK(mid);
            CHECK(before);
            CHECK(after);
        }
        CHECK(near(at(frames - kD8 - 1), lv[3]));
    }
}

/* a seek, a pause and a skip while a fade runs (a paced output: what is heard lags what is written) */
void test_engine_interrupts(const std::string &dir) {
    write_file(dir + "/a3.wav", dc_wav(44100, 44100 * 3, 8000, 4000));
    write_file(dir + "/b3.wav", dc_wav(44100, 44100 * 3, 2000, 1000));
    write_file(dir + "/c3.wav", dc_wav(44100, 44100 * 3, 500, 250));
    auto setup = [&](Engine &e) {
        e.set_crossfade_s(5); /* 1.5 s here: half of a 3 s track */
        e.load(0, dir + "/a3.wav", 0, true);
        e.set_next(1, dir + "/b3.wav", true);
    };
    auto drain_events = [](Engine &e) {
        EngineEvent ev;
        while (e.poll_event(ev)) {
        }
    };
    /* a seek while the incoming track is heard: the fade ends, the incoming track goes on alone from there */
    {
        Record r;
        r.speed = 4.0;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        setup(e);
        CHECK(wait_until([&] {
            const EngineSnapshot s = e.snapshot();
            return s.fading && s.pos == 1;
        }));
        CHECK_EQ(e.snapshot().fade_s, 5); /* the running fade's setting seconds (Now playing's chip) */
        e.seek_ms(500);
        CHECK(wait_until([&] { return !e.snapshot().fading; }, 1000));
        CHECK_EQ(e.snapshot().fade_s, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        const EngineSnapshot s = e.snapshot();
        CHECK_EQ(s.pos, 1);
        CHECK(s.frame >= 22050 && s.frame < 22050 + 44100);
        size_t at_seek = 0;
        {
            std::lock_guard<std::mutex> l(r.mu);
            at_seek = r.samples.size();
        }
        std::vector<EngineEvent> seen;
        CHECK(wait_end(e, seen));
        std::lock_guard<std::mutex> l(r.mu);
        /* after the seek only B: every sample from there on is B's level (the limiter's delay line refilled with B) */
        bool only_b = r.samples.size() > at_seek + 4000;
        for (size_t i = at_seek + 2 * (kD + 2048); only_b && i + 1 < r.samples.size(); i += 2)
            only_b = (r.samples[i] == 0 || near(r.samples[i], 2000)) && (r.samples[i + 1] == 0 || near(r.samples[i + 1], 1000));
        CHECK(only_b);
    }
    /* a pause while the incoming track is heard: paused there, the fade ended; play goes on with B alone to its end */
    {
        Record r;
        r.speed = 4.0;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        setup(e);
        CHECK(wait_until([&] {
            const EngineSnapshot s = e.snapshot();
            return s.fading && s.pos == 1 && s.frame > 4410;
        }));
        e.pause();
        CHECK(wait_until([&] { return e.snapshot().state == PlayState::Paused; }, 1000));
        const EngineSnapshot s = e.snapshot();
        CHECK(!s.fading);
        CHECK_EQ(s.pos, 1);
        const uint64_t held = s.frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK_EQ(e.snapshot().frame, held);
        drain_events(e);
        e.play();
        std::vector<EngineEvent> seen;
        CHECK(wait_end(e, seen));
        CHECK_EQ(e.snapshot().pos, 1);
    }
    /* a pause while the outgoing track is still the one heard (the mix written, not heard yet: an output half a second
     * behind): back on A at the heard frame, B named again with its crossfade: on play the fade starts again (shorter:
     * what is left of A) */
    {
        Record r;
        r.speed = 2.0;
        r.lead = 0.5;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        setup(e);
        CHECK(wait_until([&] {
            const EngineSnapshot s = e.snapshot();
            return s.fading && s.pos == 0;
        }));
        e.pause();
        CHECK(wait_until([&] { return e.snapshot().state == PlayState::Paused; }, 1000));
        const EngineSnapshot s = e.snapshot();
        CHECK_EQ(s.pos, 0);
        CHECK(!s.fading);
        CHECK(s.frame >= 44000 && s.frame <= 66150);
        drain_events(e);
        e.play();
        CHECK(wait_until([&] { return e.snapshot().fading; }, 3000));
        std::vector<EngineEvent> seen;
        CHECK(wait_end(e, seen));
        CHECK_EQ(e.snapshot().pos, 1);
    }
    /* a skip (a load) during a fade: an immediate change, no fade */
    {
        Record r;
        r.speed = 4.0;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        setup(e);
        CHECK(wait_until([&] {
            const EngineSnapshot s = e.snapshot();
            return s.fading && s.pos == 1;
        }));
        e.load(2, dir + "/c3.wav", 0, true);
        CHECK(wait_until([&] {
            const EngineSnapshot s = e.snapshot();
            return !s.fading && s.pos == 2;
        }, 1000));
        std::vector<EngineEvent> seen;
        CHECK(wait_end(e, seen));
        CHECK_EQ(e.snapshot().pos, 2);
    }
}

/* the equalizer through the engine */
void test_engine_eq(const std::string &dir) {
    write_file(dir + "/k1.wav", tone_wav(44100, 44100, 1000, 0.25));
    write_file(dir + "/k1q.wav", tone_wav(44100, 44100, 1000, 0.1)); /* -20 dBFS: +12 dB stays under the limiter's ceiling */
    write_file(dir + "/b62.wav", tone_wav(44100, 44100, 62.5, 0.25));
    write_file(dir + "/sq.wav", tone_wav(44100, 44100, 100, 1.0, true));
    auto run = [&](const std::string &f, const EqGains &g, std::vector<int16_t> &s) {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.set_eq(g);
        e.load(0, f, 0, true);
        std::vector<EngineEvent> seen;
        const bool ended = wait_end(e, seen);
        std::lock_guard<std::mutex> l(r.mu);
        s = r.samples;
        return ended && e.snapshot().eq_bands >= 0;
    };
    auto rms_db = [](const std::vector<int16_t> &s, size_t from, size_t to) {
        double a = 0;
        size_t n = 0;
        for (size_t i = from; i < to && i < s.size(); i += 2, ++n) a += static_cast<double>(s[i]) * s[i];
        return 10 * std::log10(a / std::max<size_t>(1, n) + 1e-12);
    };
    std::vector<int16_t> flat, cut, boost, sq, flat_q, flat_b, other;
    /* flat: the bypass, the integer path, bit-exact (no limiter delay) */
    CHECK(run(dir + "/k1.wav", EqGains{}, flat));
    CHECK_EQ(flat.size(), static_cast<size_t>(44100 * 2));
    const std::string raw = read_file(dir + "/k1.wav");
    bool exact = flat.size() == static_cast<size_t>(44100 * 2) && raw.size() >= 44 + flat.size() * 2;
    for (size_t i = 0; exact && i < flat.size(); i += 101)
        exact = flat[i] == static_cast<int16_t>(static_cast<uint8_t>(raw[44 + 2 * i]) | (static_cast<uint8_t>(raw[45 + 2 * i]) << 8));
    CHECK(exact);
    /* the 1 kHz band at -12 dB: a 1 kHz sine 12 dB lower */
    EqGains g{};
    g[5] = -12;
    CHECK(run(dir + "/k1.wav", g, cut));
    CHECK_EQ(cut.size(), (44100 + kD) * 2);
    const double d_cut = rms_db(cut, 2 * 11025, 2 * 44100) - rms_db(flat, 2 * 11025, 2 * 44100);
    std::fprintf(stderr, "  EQ through the engine: 1 kHz band -12 dB: %+.2f dB on a 1 kHz sine\n", d_cut);
    CHECK_NEAR(d_cut, -12.0, 0.2);
    /* +12 dB there (the owner's board test of the 0.7.0 draft: a boost must be heard as a boost): a 1 kHz sine at -20 dBFS
     * comes out 12 dB louder (under the limiter's ceiling: nothing takes it back), and a 62.5 Hz sine, four octaves away,
     * keeps its level (+-0.1 dB: no pre-gain lowers the rest of the music) */
    g[5] = 12;
    CHECK(run(dir + "/k1q.wav", EqGains{}, flat_q));
    CHECK(run(dir + "/k1q.wav", g, boost));
    const double d_boost = rms_db(boost, 2 * 11025, 2 * 44100) - rms_db(flat_q, 2 * 11025, 2 * 44100);
    CHECK(run(dir + "/b62.wav", EqGains{}, flat_b));
    CHECK(run(dir + "/b62.wav", g, other));
    const double d_other = rms_db(other, 2 * 11025, 2 * 44100) - rms_db(flat_b, 2 * 11025, 2 * 44100);
    std::fprintf(stderr, "  EQ through the engine: 1 kHz band +12 dB: %+.2f dB on a 1 kHz sine, %+.3f dB on a 62.5 Hz sine\n", d_boost,
                 d_other);
    CHECK_NEAR(d_boost, 12.0, 0.2);
    CHECK_NEAR(d_other, 0.0, 0.1);
    /* the strongest curve (every band +12 dB) on a full-scale square wave: never above the limiter's ceiling */
    EqGains maxg;
    maxg.fill(kEqMaxDb);
    CHECK(run(dir + "/sq.wav", maxg, sq));
    int peak = 0;
    for (int16_t v : sq) peak = std::max(peak, std::abs(static_cast<int>(v)));
    std::fprintf(stderr, "  EQ max boost on a full-scale square: peak %d (ceiling 29204)\n", peak);
    CHECK(peak <= 29206);
    CHECK(peak > 1000);
}

/* the FFmpeg tier (the owner's .m4a files): the EQ and a crossfade on the same decoded samples */
void test_tier2(void) {
    const char *fx = std::getenv("VINYL_FIXTURES");
    const std::string path = std::string(fx ? fx : "") + "/sine.m4a";
    FILE *f = fx && *fx ? std::fopen(path.c_str(), "rb") : nullptr;
    if (!f) {
        SKIP("EQ and crossfade through FFmpeg: no sine.m4a fixture");
        return;
    }
    std::fclose(f);
    auto run = [&](const EqGains &g, int fade, bool twice, size_t &frames) {
        Record r;
        FakeProvider p(r);
        Engine e(real_decode_libs(), p);
        e.set_eq(g);
        e.set_crossfade_s(fade);
        e.load(0, path, 0, true);
        if (twice) e.set_next(1, path, true);
        std::vector<EngineEvent> seen;
        const bool ended = wait_end(e, seen, 20000);
        std::lock_guard<std::mutex> l(r.mu);
        frames = r.samples.size() / 2;
        if (!ended || frames < 20000) return -1.0;
        double a = 0;
        size_t n = 0;
        for (size_t i = 2 * 8000; i + 2 * 8000 < std::min(r.samples.size(), static_cast<size_t>(2 * 200000)); i += 2, ++n)
            a += static_cast<double>(r.samples[i]) * r.samples[i];
        return 10 * std::log10(a / std::max<size_t>(1, n) + 1e-9);
    };
    size_t f0 = 0, f1 = 0, f2 = 0;
    const double r0 = run(EqGains{}, 0, false, f0);
    EqGains g{};
    g[4] = -12; /* 500 Hz, the 440 Hz sine near it */
    const double r1 = run(g, 0, false, f1);
    if (r0 < 0 || r1 < 0) {
        SKIP("EQ and crossfade through FFmpeg: the FFmpeg libraries are not available here");
        return;
    }
    const double want = biquad_response_db(biquad_peaking(44100, 500, kEqQ, -12), 440, 44100);
    std::fprintf(stderr, "  EQ through FFmpeg (sine.m4a, 500 Hz -12 dB): %+.2f dB, the filter says %+.2f dB at 440 Hz\n", r1 - r0, want);
    CHECK_NEAR(r1 - r0, want, 0.4);
    const double r2 = run(EqGains{}, 5, true, f2);
    /* two tracks, 5 s asked (half of the 6 s track: about 3 s) overlapped, the float path's delay */
    const double expect = 2.0 * static_cast<double>(f0) - std::min(5.0 * 44100, static_cast<double>(f0) / 2) + kD;
    std::fprintf(stderr, "  crossfade through FFmpeg: %zu frames for two tracks of %zu (expected about %.0f)\n", f2, f0, expect);
    CHECK(r2 > 0);
    CHECK(std::fabs(static_cast<double>(f2) - expect) < 4096);
}

}  // namespace

void test_crossfade() {
    test_math();
    const std::string dir = make_temp_dir("xfade");
    test_engine_fades(dir);
    test_engine_queue(dir);
    test_engine_interrupts(dir);
    test_engine_eq(dir);
    test_tier2();
    rm_tree(dir);
}
