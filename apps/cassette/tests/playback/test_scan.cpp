/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "player.hpp"
#include "support.hpp"

#include <cerrno>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

std::vector<PlayerEvent> events(Player &p) {
    std::vector<PlayerEvent> v;
    PlayerEvent e;
    while (p.poll_event(e)) v.push_back(e);
    return v;
}

/* Expected forward cue output from source frame `from`, `nout` frames, groups of g (partial only at `len`). */
std::vector<int32_t> fwd_cue(const std::vector<int32_t> &s, unsigned ch, uint64_t len, uint64_t from, uint64_t nout,
                             unsigned g = 5) {
    std::vector<int32_t> v;
    for (uint64_t j = 0; j < nout; ++j) {
        uint64_t a = from + j * g, b = std::min<uint64_t>(len, a + g);
        for (unsigned c = 0; c < ch; ++c) v.push_back(box(s, ch, c, a, b));
    }
    return v;
}

/* Expected reverse cue output from source frame `from` going back, `nout` frames (partial only at 0). */
std::vector<int32_t> rev_cue(const std::vector<int32_t> &s, unsigned ch, uint64_t from, uint64_t nout, unsigned g = 5) {
    std::vector<int32_t> v;
    for (uint64_t j = 0; j < nout; ++j) {
        uint64_t b = from - j * g, a = b > g ? b - g : 0;
        for (unsigned c = 0; c < ch; ++c) v.push_back(box(s, ch, c, a, b));
    }
    return v;
}

} // namespace

void test_scan() {
    TempDir d;
    /* forward: output frames x 5 = position change; the output is the 5-frame box average of the ramp (mono and stereo) */
    for (uint16_t ch : {uint16_t(1), uint16_t(2)}) {
        const uint64_t len = 100000;
        const auto s = ramp_samples(len, ch);
        const std::string path = d.file("fwd" + std::to_string(ch) + ".wav");
        write_wav(path, afmt(48000, ch, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        p.step();
        p.step();
        const uint64_t start = p.position();
        CHECK_EQ(start, 2048u);
        const size_t before = sink.heard_frames();
        CHECK_EQ(p.scan(Direction::Forward), 0);
        CHECK(p.state() == PlayerState::Scanning);
        CHECK(p.state_before_scan() == PlayerState::Playing);
        for (int i = 0; i < 3; ++i) p.step();
        const uint64_t out = sink.heard_frames() - before;
        CHECK_EQ(out, 3u * 1024u);
        CHECK_EQ(p.position() - start, 5 * out); /* exactly 5x */
        auto h = sink.heard();
        std::vector<int32_t> got(h.begin() + static_cast<long>(before * ch), h.end());
        CHECK(got == fwd_cue(s, ch, len, start, out));
        /* release: back to normal play from where the scan got to */
        const uint64_t here = p.position();
        CHECK_EQ(p.end_scan(), 0);
        CHECK(p.state() == PlayerState::Playing);
        const size_t b2 = sink.heard_frames();
        p.step();
        h = sink.heard();
        CHECK(std::vector<int32_t>(h.begin() + static_cast<long>(b2 * ch), h.end()) == slice(s, ch, here, here + 1024));
    }
    /* reverse: blocks read backwards, groups emitted last-first */
    {
        const uint64_t len = 100000;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("rev.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(p.pause(), 0);
        CHECK_EQ(p.seek(60000), 0);
        CHECK_EQ(p.scan(Direction::Reverse), 0);
        CHECK(p.state_before_scan() == PlayerState::Paused);
        for (int i = 0; i < 4; ++i) p.step();
        const uint64_t out = sink.heard_frames();
        CHECK_EQ(out, 4u * 1024u);
        CHECK_EQ(60000 - p.position(), 5 * out);
        CHECK(sink.heard() == rev_cue(s, 1, 60000, out));
        CHECK_EQ(p.end_scan(), 0);
        CHECK(p.state() == PlayerState::Paused); /* it was paused */
        CHECK_EQ(p.position(), 60000u - 5 * 4096u);
    }
    /* a direction change while scanning, and device latency: the queued cue is dropped and the position rewound */
    {
        const uint64_t len = 100000;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("turn.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.latency = 200;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.seek(50000), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(p.scan(Direction::Forward), 0);
        p.step();
        CHECK_EQ(p.position(), 50000u + 5 * (1024u - 200u)); /* the frame being heard */
        CHECK_EQ(p.scan(Direction::Reverse), 0);
        CHECK_EQ(p.position(), 50000u + 5 * 824u);
        CHECK(p.scan_direction() == Direction::Reverse);
        CHECK(p.state_before_scan() == PlayerState::Playing);
        CHECK_EQ(p.end_scan(), 0);
        CHECK(p.state() == PlayerState::Playing);
    }
    /* reaching the end: ReachedEnd, Stopped at the end; a last group of fewer than 5 frames is averaged over what it has */
    {
        const uint64_t len = 7003;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("end.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.seek(5000), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(p.scan(Direction::Forward), 0);
        int steps = 0;
        while (p.step() && steps < 100) ++steps;
        CHECK(p.state() == PlayerState::Stopped);
        CHECK_EQ(p.position(), len);
        auto ev = events(p);
        CHECK(ev.size() == 1 && ev[0] == PlayerEvent::ReachedEnd);
        const uint64_t out = sink.heard_frames();
        CHECK_EQ(out, (len - 5000 + 4) / 5); /* 401: 400 groups of 5 and one of 3 */
        CHECK(sink.heard() == fwd_cue(s, 1, len, 5000, out));
        CHECK_EQ(sink.heard().back(), box(s, 1, 0, 7000, 7003));
        CHECK_EQ(p.end_scan(), 0); /* the key released after the end: nothing to do */
        CHECK(p.state() == PlayerState::Stopped);
    }
    /* reaching the start: ReachedStart once, held at 0 while the key is down; release plays from the start */
    {
        const uint64_t len = 20000;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("start.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.seek(2002), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(p.scan(Direction::Reverse), 0);
        p.step(); /* 401 output frames: 400 groups and the 2 frames at the start */
        CHECK_EQ(p.position(), 0u);
        CHECK(sink.heard() == rev_cue(s, 1, 2002, 401));
        CHECK_EQ(sink.heard().back(), box(s, 1, 0, 0, 2));
        CHECK(p.step()); /* the start */
        CHECK(p.at_start_hold());
        CHECK(p.state() == PlayerState::Scanning);
        CHECK(!p.step()); /* held: nothing more */
        auto ev = events(p);
        CHECK(ev.size() == 1 && ev[0] == PlayerEvent::ReachedStart);
        CHECK_EQ(p.end_scan(), 0);
        CHECK(p.state() == PlayerState::Playing);
        sink.clear_heard();
        p.step();
        CHECK(sink.heard() == slice(s, 1, 0, 1024));
    }
    /* a scan of a cued (stopped) take counts as paused; speeds other than 5 */
    {
        const uint64_t len = 30000;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("cued.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.scan(Direction::Forward, 3), 0);
        CHECK(p.state_before_scan() == PlayerState::Paused);
        p.step();
        CHECK_EQ(p.position(), 3u * 1024u);
        CHECK(sink.heard() == fwd_cue(s, 1, len, 0, 1024, 3));
        CHECK_EQ(p.end_scan(), 0);
        CHECK(p.state() == PlayerState::Paused);
        CHECK_EQ(p.play(), 0);
        CHECK(p.state() == PlayerState::Playing);
        CHECK_EQ(p.scan(Direction::Forward), 0);
        CHECK_EQ(p.pause(), 0); /* pause ends a scan */
        CHECK(p.state() == PlayerState::Paused);
        CHECK_EQ(p.scan(Direction::Forward), 0);
        CHECK_EQ(p.play(), 0); /* play ends a scan */
        CHECK(p.state() == PlayerState::Playing);
        CHECK_EQ(p.stop(), 0);
        CHECK(p.state() == PlayerState::Stopped);
        CHECK_EQ(p.end_scan(), 0);
    }
}
