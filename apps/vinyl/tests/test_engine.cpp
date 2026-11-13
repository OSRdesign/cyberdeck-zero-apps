/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the engine on WAV files and a fake output: gapless joins (the frames written are the tracks' frames, back
 * to back, on one stream), a format change (drain and a new stream), pause and play, seek, the end of the queue, a next
 * track that cannot be opened, the loss of the output (pause at the frame heard, never another output) and a profile
 * switch (the same device found again), the heard-position events.
 */
#include "check.hpp"

#include "engine/engine.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

using namespace vinyl;

namespace {

struct Record {
    std::mutex mu;
    std::vector<int16_t> samples; /* every S16 sample written, all streams */
    int opens = 0, drains = 0, drops = 0, closes = 0, provider_opens = 0;
    std::vector<uint32_t> rates;
    long fail_after = -1;   /* writes before -ENODEV */
    bool still_there = false;
    double speed = 0;       /* 0: instant; else frames per second heard x speed (a paced output) */
    long fixed_delay = 0;
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
            std::lock_guard<std::mutex> l(r_.mu);
            if (r_.fail_after == 0) {
                if (r_.still_there) r_.fail_after = -1; /* a profile switch: the next stream works */
                return -ENODEV;
            }
            if (r_.fail_after > 0) --r_.fail_after;
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
    int open(const OutputTarget &t, OpenedSink &o, std::string &) override {
        {
            std::lock_guard<std::mutex> l(r_.mu);
            ++r_.provider_opens;
        }
        o.sink = std::make_unique<FakeSink>(r_);
        o.name = t.kind == OutputTarget::Kind::Auto ? "default" : t.id;
        o.label = "Fake output";
        o.device = o.name;
        return 0;
    }
    bool still_there(const OpenedSink &) override { return r_.still_there; }

private:
    Record &r_;
};

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

bool wait_for(Engine &e, EngineEvent::Kind kind, std::vector<EngineEvent> &seen, int ms = 3000) {
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

}  // namespace

void test_engine() {
    const std::string dir = make_temp_dir("engine");
    write_file(dir + "/a.wav", ramp_wav(44100, 5000, 0));
    write_file(dir + "/b.wav", ramp_wav(44100, 3000, 5000));
    write_file(dir + "/c.wav", ramp_wav(48000, 2000, 20000));
    write_file(dir + "/long.wav", ramp_wav(44100, 44100 * 4, 0)); /* 4 s */

    /* gapless: a then b on one stream, the samples back to back */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        e.set_next(1, dir + "/b.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_for(e, EngineEvent::Kind::QueueEnd, seen));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 1);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(8000 * 2));
        bool exact = r.samples.size() == 16000;
        for (size_t i = 0; exact && i < 8000; ++i)
            exact = r.samples[2 * i] == static_cast<int16_t>(i) && r.samples[2 * i + 1] == static_cast<int16_t>(-static_cast<int>(i));
        CHECK(exact);
        CHECK_EQ(r.drains, 1); /* only at the end of the queue */
        bool adv = false, started_b = false;
        for (const EngineEvent &ev : seen) {
            if (ev.kind == EngineEvent::Kind::Advanced && ev.pos == 1) adv = true;
            if (ev.kind == EngineEvent::Kind::Started && ev.pos == 1) started_b = adv; /* heard after the decoder moved */
        }
        CHECK(adv);
        CHECK(started_b);
        const EngineSnapshot s = e.snapshot();
        CHECK(s.state == PlayState::Paused);
        CHECK_EQ(s.pos, 1);
        CHECK_EQ(s.frame, static_cast<uint64_t>(0)); /* back at the start of the last track */
    }
    /* another rate: the first stream is drained, a new one opened at 48 kHz */
    {
        Record r;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/a.wav", 0, true);
        e.set_next(1, dir + "/c.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_for(e, EngineEvent::Kind::QueueEnd, seen));
        std::lock_guard<std::mutex> l(r.mu);
        CHECK_EQ(r.opens, 2);
        CHECK(r.rates.size() == 2 && r.rates[0] == 44100 && r.rates[1] == 48000);
        CHECK_EQ(r.drains, 2);
        CHECK_EQ(r.samples.size(), static_cast<size_t>(7000 * 2));
    }
    /* pause, play, seek on a paced output */
    {
        Record r;
        r.speed = 1.0;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/long.wav", 1000, false);
        CHECK(wait_state(e, PlayState::Paused));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EngineSnapshot s = e.snapshot();
        CHECK_EQ(s.frame, static_cast<uint64_t>(44100)); /* loaded paused at 1 s */
        CHECK_EQ(s.total, static_cast<uint64_t>(44100 * 4));
        CHECK_EQ(r.provider_opens, 0);                    /* nothing opened while paused */
        e.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        s = e.snapshot();
        CHECK(s.state == PlayState::Playing);
        CHECK(s.frame > 44100 + 4410 && s.frame < 44100 + 44100);
        e.pause();
        CHECK(wait_state(e, PlayState::Paused));
        const uint64_t held = e.snapshot().frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_EQ(e.snapshot().frame, held); /* the heard frame is kept */
        e.seek_ms(3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(3 * 44100));
        e.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(e.snapshot().frame > static_cast<uint64_t>(3 * 44100));
        e.stop();
        CHECK(wait_state(e, PlayState::Paused));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK_EQ(e.snapshot().frame, static_cast<uint64_t>(0));
    }
    /* the output goes away: paused at the frame heard, OutputLost, no other output opened */
    {
        Record r;
        r.speed = 4.0;
        r.fail_after = 20;
        r.still_there = false;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        std::vector<EngineEvent> seen;
        CHECK(wait_for(e, EngineEvent::Kind::OutputLost, seen, 6000));
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
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        std::vector<EngineEvent> seen;
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
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/b.wav", 0, true);
        e.set_next(1, dir + "/missing.wav");
        std::vector<EngineEvent> seen;
        CHECK(wait_for(e, EngineEvent::Kind::OpenFailed, seen));
        CHECK(seen.back().automatic);
        CHECK_EQ(seen.back().pos, 1);
        CHECK(!seen.back().text.empty());
        CHECK(wait_state(e, PlayState::Idle));
        /* a load of a missing file: OpenFailed, not automatic */
        e.load(3, dir + "/none.wav", 0, true);
        seen.clear();
        CHECK(wait_for(e, EngineEvent::Kind::OpenFailed, seen));
        CHECK(!seen.back().automatic);
    }
    /* the output chosen while playing: the stream is reopened on the new one */
    {
        Record r;
        r.speed = 2.0;
        FakeProvider p(r);
        Engine e(DecodeLibs(), p);
        e.load(0, dir + "/long.wav", 0, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        OutputTarget t;
        t.kind = OutputTarget::Kind::Pulse;
        t.id = "other";
        e.set_output(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK_EQ(r.provider_opens, 2);
        CHECK_EQ(e.snapshot().output_name, std::string("other"));
        CHECK(e.snapshot().state == PlayState::Playing);
    }
    rm_tree(dir);
}
