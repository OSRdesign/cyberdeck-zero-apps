/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "player.hpp"
#include "support.hpp"

#include <cerrno>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

void run_to_end(Player &p, int max_steps = 100000) {
    while (max_steps-- > 0 && p.step()) {
    }
}

std::vector<PlayerEvent> events(Player &p) {
    std::vector<PlayerEvent> v;
    PlayerEvent e;
    while (p.poll_event(e)) v.push_back(e);
    return v;
}

void reader_cases() {
    TempDir d;
    const AudioFormat f = afmt(48000, 1, 16);
    const auto s = ramp_samples(30000, 1);
    struct Case {
        HeaderKind kind;
        uint64_t extra;
        uint64_t frames;
        bool distrust;
    } cases[] = {{HeaderKind::Final, 0, 30000, false},
                 {HeaderKind::Placeholder, 0, 30000, true},
                 {HeaderKind::Oversized, 5000, 30000, true},
                 {HeaderKind::Short, 1000, 29000, false}};
    int n = 0;
    for (const Case &c : cases) {
        const std::string path = d.file("r" + std::to_string(n++) + ".wav");
        write_wav(path, f, s, c.kind, c.extra);
        WavReader r(posix_file_ops());
        CHECK_EQ(r.open(path), 0);
        CHECK_EQ(r.frames(), c.frames);
        CHECK_EQ(r.used_file_length(), c.distrust);
        CHECK_EQ(r.format().rate, 48000u);
        std::vector<int16_t> buf(1000);
        CHECK_EQ(r.read(c.frames - 400, 1000, buf.data()), 400L); /* stops at the playable end */
        CHECK_EQ(static_cast<int32_t>(buf[399]), s[c.frames - 1]);
        CHECK_EQ(r.read(c.frames, 10, buf.data()), 0L);
    }
    /* a file with a partial last frame (stereo, one stray byte pair) */
    {
        const std::string path = d.file("odd.wav");
        auto st = ramp_samples(1001, 2);
        st.pop_back(); /* 1000 whole frames + half a frame */
        write_wav(path, afmt(48000, 2, 16), st, HeaderKind::Placeholder);
        WavReader r(posix_file_ops());
        CHECK_EQ(r.open(path), 0);
        CHECK_EQ(r.frames(), 1000u);
    }
    /* not a WAV */
    {
        const std::string path = d.file("junk.wav");
        std::ofstream(path) << "this is not a wave file at all, just text";
        WavReader r(posix_file_ops());
        CHECK(r.open(path) < 0);
        CHECK(!r.is_open());
        CHECK(r.open(d.file("missing.wav")) < 0);
    }
}

} // namespace

void test_reader() { reader_cases(); }

void test_player() {
    TempDir d;
    /* 1. exact samples, mono 16-bit, a length that is not a whole number of periods */
    {
        const AudioFormat f = afmt(48000, 1, 16);
        const auto s = ramp_samples(48000 + 333, 1);
        const std::string path = d.file("mono.wav");
        write_wav(path, f, s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.play(), -ENOENT); /* nothing open */
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.length(), 48333u);
        CHECK(p.length_s() > 1.0069 && p.length_s() < 1.0070);
        CHECK(p.state() == PlayerState::Stopped);
        CHECK_EQ(p.play(), 0);
        CHECK(p.state() == PlayerState::Playing);
        CHECK_EQ(sink.format().buffer_frames, 24576u); /* about 500 ms */
        run_to_end(p);
        CHECK(sink.heard() == s);
        CHECK(p.state() == PlayerState::Stopped);
        CHECK_EQ(p.position(), p.length());
        auto ev = events(p);
        CHECK(ev.size() == 1 && ev[0] == PlayerEvent::TakeEnded);
        CHECK_EQ(sink.drains(), 1u);
        CHECK(!sink.is_open());
        /* play again after the end: from the start */
        sink.clear_heard();
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(sink.heard() == s);
    }
    /* 2. stereo 24-bit and 32-bit, short writes */
    for (uint16_t bits : {uint16_t(24), uint16_t(32)}) {
        const AudioFormat f = afmt(44100, 2, bits);
        const auto s = ramp_samples(7000, 2, bits);
        const std::string path = d.file("st" + std::to_string(bits) + ".wav");
        write_wav(path, f, s);
        FakeSink sink;
        sink.max_write = 300;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(sink.heard() == s);
    }
    /* 3. mono to a device that needs 2 channels: the channel duplicated; and a 24-bit take on an S16-only device */
    {
        const auto s = ramp_samples(5000, 1);
        const std::string path = d.file("dup.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.force_channels = 2;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        std::vector<int32_t> want;
        for (int32_t v : s) {
            want.push_back(v);
            want.push_back(v);
        }
        CHECK(sink.heard() == want);
        CHECK_EQ(p.sink_format().channels, 2u);

        const auto s24 = ramp_samples(3000, 1, 24);
        const std::string p24 = d.file("s24.wav");
        write_wav(p24, afmt(48000, 1, 24), s24);
        FakeSink s16;
        s16.force_s16 = true;
        Player q(posix_file_ops(), s16, Player::Mode::Manual);
        CHECK_EQ(q.open(p24), 0);
        CHECK_EQ(q.play(), 0);
        run_to_end(q);
        auto h = s16.heard();
        bool ok = h.size() == s24.size();
        for (size_t i = 0; ok && i < h.size(); ++i) ok = h[i] == (s24[i] >> 8);
        CHECK(ok);
    }
    /* 4. pause and resume lose and repeat nothing, with and without device latency */
    for (long latency : {0L, 700L, 5000L}) {
        const auto s = ramp_samples(20000, 1);
        const std::string path = d.file("pause.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.latency = latency;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        for (int i = 0; i < 6; ++i) p.step();
        const uint64_t sent = 6 * 1024;
        const uint64_t heard_pos = sent - std::min<uint64_t>(sent, static_cast<uint64_t>(latency));
        CHECK_EQ(p.position(), heard_pos); /* the frame being heard */
        CHECK_EQ(p.pause(), 0);
        CHECK(p.state() == PlayerState::Paused);
        CHECK_EQ(p.position(), heard_pos);
        CHECK_EQ(sink.heard_frames(), heard_pos);
        CHECK(!p.step()); /* nothing plays while paused */
        CHECK_EQ(p.resume(), 0);
        CHECK(p.state() == PlayerState::Playing);
        p.step();
        CHECK_EQ(p.pause(), 0); /* a second pause after one block */
        CHECK_EQ(p.resume(), 0);
        run_to_end(p);
        CHECK(sink.heard() == s);
    }
    /* 5. seek is exact (playing, paused, stopped, clamped) */
    {
        const auto s = ramp_samples(30000, 1);
        const std::string path = d.file("seek.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.latency = 300;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        for (int i = 0; i < 3; ++i) p.step();
        CHECK_EQ(p.seek(10000), 0);
        CHECK_EQ(p.position(), 10000u);
        CHECK(p.state() == PlayerState::Playing);
        p.step();
        CHECK_EQ(p.pause(), 0);
        CHECK_EQ(p.seek(25000), 0);
        CHECK_EQ(p.position(), 25000u);
        CHECK(p.state() == PlayerState::Paused);
        CHECK_EQ(p.resume(), 0);
        run_to_end(p);
        std::vector<int32_t> want = slice(s, 1, 0, 3 * 1024 - 300);
        auto mid = slice(s, 1, 10000, 10000 + 1024 - 300);
        want.insert(want.end(), mid.begin(), mid.end());
        auto tail = slice(s, 1, 25000, 30000);
        want.insert(want.end(), tail.begin(), tail.end());
        CHECK(sink.heard() == want);
        CHECK_EQ(p.seek(999999), 0);
        CHECK_EQ(p.position(), 30000u); /* clamped */
        CHECK_EQ(p.stop(), 0);
        CHECK_EQ(p.position(), 0u); /* stop rewinds */
        CHECK_EQ(p.seek(12345), 0);  /* seek while stopped, then play from there */
        sink.clear_heard();
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(sink.heard() == slice(s, 1, 12345, 30000));
    }
    /* 6. a placeholder header (never updated) plays to the real end of the file */
    {
        const auto s = ramp_samples(30001, 1);
        const std::string path = d.file("ph.wav");
        write_wav(path, afmt(48000, 1, 16), s, HeaderKind::Placeholder);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.length(), 30001u);
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(sink.heard() == s);
    }
    /* 7. jump: +-5 s with clamping */
    {
        const AudioFormat f = afmt(8000, 1, 16);
        const auto s = ramp_samples(8000 * 12, 1);
        const std::string path = d.file("jump.wav");
        write_wav(path, f, s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        p.step();
        CHECK_EQ(p.position(), 1024u);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.position(), 1024u + 40000u);
        CHECK_EQ(p.jump(Direction::Reverse, 2), 0);
        CHECK_EQ(p.position(), 1024u + 40000u - 16000u);
        CHECK_EQ(p.jump(Direction::Reverse), 0);
        CHECK_EQ(p.position(), 0u); /* clamped at the start */
        CHECK(p.state() == PlayerState::Playing);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.position(), 96000u); /* clamped at the end */
        p.step();                      /* the end: the take is over */
        CHECK(p.state() == PlayerState::Stopped);
        auto ev = events(p);
        CHECK(ev.size() == 1 && ev[0] == PlayerEvent::TakeEnded);
        /* jump while paused keeps it paused */
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(p.pause(), 0);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.position(), 40000u);
        CHECK(p.state() == PlayerState::Paused);
    }
    /* 8. underruns are recovered without losing a sample; a lost device pauses at the frame reached */
    {
        const auto s = ramp_samples(12000, 1);
        const std::string path = d.file("xrun.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.underrun_at = {3000, 7000};
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        run_to_end(p);
        CHECK(sink.heard() == s);
        CHECK_EQ(p.underruns(), 2u);

        FakeSink lost;
        lost.lost_at = 5000;
        Player q(posix_file_ops(), lost, Player::Mode::Manual);
        CHECK_EQ(q.open(path), 0);
        CHECK_EQ(q.play(), 0);
        run_to_end(q);
        CHECK(q.state() == PlayerState::Paused);
        CHECK_EQ(q.position(), 5000u);
        auto ev = events(q);
        CHECK(ev.size() == 1 && ev[0] == PlayerEvent::DeviceLost);
        CHECK(!lost.is_open());
        lost.lost_at = UINT64_MAX; /* the card is back */
        CHECK_EQ(q.resume(), 0);
        CHECK_EQ(lost.opens(), 2u);
        run_to_end(q);
        CHECK(lost.heard() == s);
    }
    /* 9. errors: a sink that cannot open, a file that is not a WAV */
    {
        const auto s = ramp_samples(2000, 1);
        const std::string path = d.file("err.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.open_error = -EBUSY;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), -EBUSY);
        CHECK(p.state() == PlayerState::Stopped);
        CHECK_EQ(p.scan(Direction::Forward), -EBUSY);
        CHECK(p.state() == PlayerState::Stopped);
        std::ofstream(d.file("bad.wav")) << "not a wave";
        CHECK(p.open(d.file("bad.wav")) < 0);
        CHECK_EQ(p.length(), 0u);
        CHECK_EQ(p.play(), -ENOENT);
    }
    /* 10. resume from Stopped (a cued take) plays from the position */
    {
        const auto s = ramp_samples(4000, 1);
        const std::string path = d.file("cue.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.seek(1000), 0);
        CHECK_EQ(p.resume(), 0);
        CHECK(p.state() == PlayerState::Playing);
        run_to_end(p);
        CHECK(sink.heard() == slice(s, 1, 1000, 4000));
        CHECK_EQ(p.close(), 0);
        CHECK_EQ(p.play(), -ENOENT);
    }
}
