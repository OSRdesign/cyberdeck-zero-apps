/* SPDX-License-Identifier: MIT */
/*
 * Cassette playback through the sound server (task T3): the Player on a PulsePlaybackSink over FakePulse. The "server" is a
 * thread that plays (takes) what the streams queue and records it, so every check is on the exact samples that reached the
 * fake sink: rates 8 / 16 / 44.1 / 48 kHz, mono and stereo, the position counter against the delay, underruns, pause, resume,
 * seek, the 5x scan, the 5 s jump, a sink that goes away, a sink that is not there. Synthetic ramps only.
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "player.hpp"
#include "pulse_sink.hpp"
#include "support.hpp"

#include <atomic>
#include <cerrno>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

const char *kMac = "70:BF:92:53:6B:8B";

/* The fake sound server's playback side: takes everything the playback streams hold, as a real server would play it. */
struct Server {
    FakePulse &f;
    std::atomic<bool> stop{false}, stall{false};
    std::mutex m;
    std::vector<std::vector<int16_t>> data; /* per stream index */
    std::thread th;
    explicit Server(FakePulse &p) : f(p) {
        th = std::thread([this] {
            while (!stop.load()) {
                if (!stall.load()) {
                    std::lock_guard<std::recursive_mutex> g(f.lock());
                    for (size_t i = 0; i < f.streams.size(); ++i) {
                        FakeStream &s = f.streams[i];
                        if (s.capture || !s.open || !s.play) continue;
                        const size_t q = s.play->queued_frames();
                        if (!q) continue;
                        std::vector<int16_t> buf(q * s.channels);
                        const size_t got = s.play->take(buf.data(), q);
                        std::lock_guard<std::mutex> g2(m);
                        if (data.size() <= i) data.resize(i + 1);
                        data[i].insert(data[i].end(), buf.begin(), buf.begin() + static_cast<long>(got * s.channels));
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~Server() {
        stop = true;
        th.join();
    }
    /* what the stream played: the server's takes, then what the end-of-take drain played out at once */
    std::vector<int16_t> got(size_t i) {
        std::vector<int16_t> v;
        {
            std::lock_guard<std::mutex> g(m);
            if (i < data.size()) v = data[i];
        }
        std::lock_guard<std::recursive_mutex> g(f.lock());
        if (i < f.streams.size()) v.insert(v.end(), f.streams[i].drained.begin(), f.streams[i].drained.end());
        return v;
    }
    void clear() {
        std::lock_guard<std::mutex> g(m);
        data.clear();
    }
};

void headset(FakePulse &f) {
    f.sinks.push_back(fake_bt_sink(kMac, 70));
    f.cards.push_back(fake_bt_card(kMac, 70, true, "a2dp-sink"));
    f.connect("test", 100);
}

std::vector<int16_t> to16(const std::vector<int32_t> &s) {
    std::vector<int16_t> v;
    for (int32_t x : s) v.push_back(static_cast<int16_t>(x));
    return v;
}

void no_latency(FakePulse &f) { /* the fake server reports no latency of its own: delay = what the ring holds */
    std::lock_guard<std::recursive_mutex> g(f.lock());
    for (FakeStream &st : f.streams) st.latency_us = 0;
}

void run_to_end(Player &p) {
    int n = 100000;
    while (n-- > 0 && p.step()) {
    }
}

bool wait_for(const std::function<bool()> &c, int ms = 2000) {
    for (int i = 0; i < ms; ++i) {
        if (c()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return c();
}

/* the exact samples at the take's rate and channel count: 8 / 16 / 44.1 / 48 kHz, mono and stereo */
void t_rates_exact() {
    TempDir d;
    struct Case { uint32_t rate; uint16_t ch; };
    for (Case c : {Case{8000, 1}, Case{16000, 1}, Case{16000, 2}, Case{44100, 2}, Case{48000, 1}, Case{48000, 2}}) {
        FakePulse f;
        headset(f);
        Server srv(f);
        PulsePlaybackSink sink(f);
        sink.set_sink(f.sinks[0].name);
        const uint64_t len = c.rate / 2 + 37; /* half a second and a partial period */
        const auto s = ramp_samples(len, c.ch);
        const std::string path = d.file("r" + std::to_string(c.rate) + std::to_string(c.ch) + ".wav");
        write_wav(path, afmt(c.rate, c.ch, 16), s);
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        {
            std::lock_guard<std::recursive_mutex> g(f.lock());
            CHECK_EQ(f.streams.size(), 1u);
            CHECK_EQ(f.streams[0].requested, f.sinks[0].name);
            CHECK_EQ(f.streams[0].rate, c.rate);                         /* the TAKE's rate: the server resamples */
            CHECK_EQ(f.streams[0].channels, static_cast<unsigned>(c.ch));
            CHECK((f.streams[0].flags & 0x200u) != 0);                    /* DONT_MOVE */
        }
        CHECK_EQ(p.sink_format().rate, c.rate);
        CHECK(p.sink_format().format == SampleFormat::S16_LE);
        CHECK_EQ(p.sink_format().period_frames, std::max<uint32_t>(64, c.rate * 20 / 1000)); /* 20 ms */
        run_to_end(p);
        CHECK(wait_for([&] { return srv.got(0).size() == s.size(); }));
        CHECK(srv.got(0) == to16(s)); /* sample-exact */
        CHECK(p.state() == PlayerState::Stopped);
        PlayerEvent e;
        CHECK(p.poll_event(e) && e == PlayerEvent::TakeEnded);
        CHECK_EQ(p.underruns(), 0u);
    }
}

/* a 24-bit take reaches the sink as 16-bit (the Player converts): same length, the top 16 bits */
void t_24bit_take() {
    TempDir d;
    FakePulse f;
    headset(f);
    Server srv(f);
    PulsePlaybackSink sink(f);
    sink.set_sink(f.sinks[0].name);
    const auto s = ramp_samples(4000, 1, 24);
    const std::string path = d.file("w24.wav");
    write_wav(path, afmt(16000, 1, 24), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    run_to_end(p);
    CHECK(wait_for([&] { return srv.got(0).size() == s.size(); }));
    const auto g = srv.got(0);
    bool close_enough = g.size() == s.size();
    for (size_t i = 0; close_enough && i < g.size(); ++i) close_enough = std::abs(g[i] - s[i] / 256) <= 1;
    CHECK(close_enough);
}

/* the position counter is the frame being heard: written minus what the stream still holds (ring + the server's latency) */
void t_position_counter() {
    TempDir d;
    for (unsigned latency_us : {20000u, 0u}) {
        FakePulse f;
        headset(f);
        Server srv(f);
        srv.stall = true; /* the server takes nothing: everything written stays in the ring */
        PulsePlaybackSink sink(f);
        sink.set_sink(f.sinks[0].name);
        const auto s = ramp_samples(600, 1); /* 75 ms at 8 kHz: fits the 100 ms ring */
        const std::string path = d.file("pos.wav");
        write_wav(path, afmt(8000, 1, 16), s);
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        {
            std::lock_guard<std::recursive_mutex> g(f.lock());
            f.streams[0].latency_us = latency_us;
        }
        for (int i = 0; i < 4; ++i) p.step(); /* 160-frame periods: 600 frames in four steps (the last is partial) */
        CHECK(p.state() == PlayerState::Playing);
        CHECK_EQ(p.position(), 0u); /* 600 queued, nothing played: the counter does not run ahead of the sound */
        {   /* the server plays 400 frames: 200 stay queued, plus the server's own latency (20 ms = 160 frames, or 0) */
            std::lock_guard<std::recursive_mutex> g(f.lock());
            CHECK_EQ(f.consume(0, 400), 400u);
        }
        CHECK_EQ(p.pause(), 0); /* rewinds by the delay and publishes */
        CHECK_EQ(p.position(), latency_us ? 240u : 400u);
    }
}

/* underruns: the server's underflow is -EPIPE once, counted, recovered; playback goes on and loses nothing */
void t_underrun() {
    TempDir d;
    FakePulse f;
    headset(f);
    Server srv(f);
    PulsePlaybackSink sink(f);
    sink.set_sink(f.sinks[0].name);
    const auto s = ramp_samples(16000, 1);
    const std::string path = d.file("ur.wav");
    write_wav(path, afmt(16000, 1, 16), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    for (int i = 0; i < 10; ++i) p.step();
    CHECK_EQ(p.underruns(), 0u);
    f.underflow(0);
    f.underflow(0); /* two underflows between two writes are one -EPIPE (the counter moved) */
    for (int i = 0; i < 5; ++i) p.step();
    CHECK_EQ(p.underruns(), 1u);
    f.underflow(0);
    run_to_end(p);
    CHECK_EQ(p.underruns(), 2u);
    CHECK(wait_for([&] { return srv.got(0).size() == s.size(); }));
    CHECK(srv.got(0) == to16(s)); /* nothing lost, nothing repeated */
}

/* pause: the queue is dropped (the stream stays open), the counter rewinds by what was queued, resume continues from it; the
 * underflows of a pause are not underruns */
void t_pause_resume() {
    TempDir d;
    FakePulse f;
    headset(f);
    Server srv(f);
    PulsePlaybackSink sink(f);
    sink.set_sink(f.sinks[0].name);
    const uint64_t len = 48000;
    const auto s = ramp_samples(len, 1);
    const std::string path = d.file("pr.wav");
    write_wav(path, afmt(16000, 1, 16), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    no_latency(f);
    for (int i = 0; i < 12; ++i) p.step();
    CHECK_EQ(p.pause(), 0);
    CHECK(p.state() == PlayerState::Paused);
    const uint64_t at = p.position();
    CHECK(at > 0 && at <= 12u * 320u);
    CHECK(sink.is_open()); /* no reopen: the stream stays */
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    f.underflow(0); /* the server ran dry while paused */
    const auto before = srv.got(0);
    CHECK(before.size() >= at);                                                        /* everything up to `at` was heard */
    CHECK(std::vector<int16_t>(before.begin(), before.begin() + static_cast<long>(at)) == to16(slice(s, 1, 0, at)));
    CHECK_EQ(p.resume(), 0);
    run_to_end(p);
    CHECK_EQ(p.underruns(), 0u);
    CHECK(wait_for([&] { return srv.got(0).size() >= before.size() + (len - at); }));
    const auto all = srv.got(0);
    /* after the pause the stream continues at the counter: the tail is exactly the take from `at` */
    CHECK(std::vector<int16_t>(all.end() - static_cast<long>(len - at), all.end()) == to16(slice(s, 1, at, len)));
    CHECK_EQ(sink.is_open(), false); /* the end of the take closed it */
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.streams.size(), 1u); /* one stream for the whole take, pause included */
    }
}

/* the 5x scan and the 5 s jump at a low rate: the stream gets the 5-frame box averages, the position moves 5 per frame */
void t_scan_and_jump() {
    TempDir d;
    FakePulse f;
    headset(f);
    Server srv(f);
    PulsePlaybackSink sink(f);
    sink.set_sink(f.sinks[0].name);
    const uint64_t len = 80000; /* 10 s at 8 kHz */
    const auto s = ramp_samples(len, 1);
    const std::string path = d.file("sc.wav");
    write_wav(path, afmt(8000, 1, 16), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    no_latency(f); /* the counter then equals what was written once the server has taken it */
    p.step();
    p.step();
    const uint64_t start = 320; /* 2 periods of 160 frames */
    CHECK(wait_for([&] { return srv.got(0).size() == start; }));
    CHECK_EQ(p.scan(Direction::Forward), 0);
    for (int i = 0; i < 3; ++i) p.step();
    CHECK(wait_for([&] { return srv.got(0).size() == start + 480; }));
    std::vector<int16_t> want;
    for (uint64_t j = 0; j < 480; ++j) want.push_back(static_cast<int16_t>(box(s, 1, 0, start + j * 5, start + j * 5 + 5)));
    const auto g1 = srv.got(0);
    CHECK(std::vector<int16_t>(g1.begin() + static_cast<long>(start), g1.end()) == want); /* the box average of 5 frames */
    CHECK_EQ(p.end_scan(), 0);
    CHECK(p.state() == PlayerState::Playing);
    CHECK_EQ(p.position(), start + 480u * 5u); /* exactly 5 source frames per frame sent */
    CHECK_EQ(p.jump(Direction::Forward), 0);    /* +5 s = 40000 frames at 8 kHz */
    CHECK_EQ(p.position(), start + 2400u + 40000u);
    p.step();
    const uint64_t at = start + 2400 + 40000;
    CHECK(wait_for([&] { return srv.got(0).size() == start + 480 + 160; }));
    const auto g2 = srv.got(0);
    CHECK(std::vector<int16_t>(g2.end() - 160, g2.end()) == to16(slice(s, 1, at, at + 160)));
    CHECK_EQ(p.jump(Direction::Reverse), 0);
    CHECK_EQ(p.position(), at + 160 - 40000u);
}

/* a sink that goes away: write fails, the Player reports DeviceLost and never writes to anything else */
void t_sink_lost() {
    TempDir d;
    FakePulse f;
    headset(f);
    f.sinks.push_back(fake_usb_sink("other", 50));
    Server srv(f);
    PulsePlaybackSink sink(f);
    sink.set_sink(f.sinks[0].name);
    const auto s = ramp_samples(48000, 1);
    const std::string path = d.file("lost.wav");
    write_wav(path, afmt(16000, 1, 16), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    for (int i = 0; i < 5; ++i) p.step();
    const std::string gone = f.sinks[0].name;
    f.remove_sink(gone); /* the headset disconnects */
    CHECK(wait_for([&] { return sink.ended(); }));
    int n = 50;
    while (n-- > 0 && p.state() == PlayerState::Playing) p.step();
    CHECK(p.state() == PlayerState::Paused);
    PlayerEvent e;
    bool lost = false;
    while (p.poll_event(e))
        if (e == PlayerEvent::DeviceLost) lost = true;
    CHECK(lost);
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.streams.size(), 1u); /* never reopened elsewhere */
    }
    /* a sink name that does not exist: the open fails, nothing is attached anywhere */
    PulsePlaybackSink none(f);
    none.set_sink("bluez_output.00_00_00_00_00_00.1");
    Player q(posix_file_ops(), none, Player::Mode::Manual);
    CHECK_EQ(q.open(path), 0);
    CHECK(q.play() < 0);
    {
        std::lock_guard<std::recursive_mutex> g(f.lock());
        CHECK_EQ(f.streams.size(), 1u);
    }
}

/* a server that takes nothing for stuck_ms: the device counts as lost (no endless wait inside the Player) */
void t_stuck_server() {
    TempDir d;
    FakePulse f;
    headset(f);
    Server srv(f);
    srv.stall = true;
    PulseSinkOptions o;
    o.stuck_ms = 300;
    PulsePlaybackSink sink(f, o);
    sink.set_sink(f.sinks[0].name);
    const auto s = ramp_samples(32000, 1);
    const std::string path = d.file("stuck.wav");
    write_wav(path, afmt(16000, 1, 16), s);
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), 0);
    int n = 100;
    while (n-- > 0 && p.state() == PlayerState::Playing) p.step();
    CHECK(p.state() == PlayerState::Paused); /* DeviceLost */
}

} // namespace

void test_pulse_sink() {
    t_rates_exact();
    t_24bit_take();
    t_position_counter();
    t_underrun();
    t_pause_resume();
    t_scan_and_jump();
    t_sink_lost();
    t_stuck_server();
}
