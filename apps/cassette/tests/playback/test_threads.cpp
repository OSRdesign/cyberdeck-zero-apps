/* SPDX-License-Identifier: MIT */
/* The playback thread with commands from other threads (run under TSan: tests/playback/run.sh SAN=thread). */
#include "check.hpp"
#include "player.hpp"
#include "support.hpp"

#include <atomic>
#include <cerrno>
#include <random>
#include <thread>

using namespace cassette;
using namespace tp;

void test_threads() {
    TempDir d;
    /* threaded play of a whole file: exact samples, TakeEnded */
    {
        const auto s = ramp_samples(50000, 1);
        const std::string path = d.file("t.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        PlayerEvent e{};
        CHECK(wait_until([&] { return p.poll_event(e); }, 5000));
        CHECK(e == PlayerEvent::TakeEnded);
        CHECK(sink.heard() == s);
        CHECK(p.state() == PlayerState::Stopped);
    }
    /* 200 loops of seek / pause / resume / scan / jump from this thread, a second thread reading the state */
    {
        const uint64_t len = 48000 * 10;
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("long.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.sleep_us = 100;
        sink.latency = 512;
        Player p(posix_file_ops(), sink);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        std::atomic<bool> stop{false};
        std::atomic<unsigned> bad{0};
        std::thread reader([&] {
            while (!stop.load()) {
                if (p.position() > p.length()) ++bad;
                (void)p.state();
                (void)p.position_s();
                (void)p.underruns();
                PlayerEvent e;
                while (p.poll_event(e)) {
                }
                std::this_thread::yield();
            }
        });
        std::mt19937 rng(1234);
        unsigned timeouts = 0, errors = 0;
        for (int i = 0; i < 200; ++i) {
            int r = 0;
            switch (rng() % 9) {
            case 0: r = p.seek(rng() % len); break;
            case 1: r = p.pause(); break;
            case 2: r = p.resume(); break;
            case 3: r = p.scan(Direction::Forward); break;
            case 4: r = p.scan(Direction::Reverse); break;
            case 5: r = p.end_scan(); break;
            case 6: r = p.jump(rng() % 2 ? Direction::Forward : Direction::Reverse); break;
            case 7: r = p.play(); break;
            case 8: std::this_thread::sleep_for(std::chrono::milliseconds(1)); break;
            }
            if (r == -ETIMEDOUT) ++timeouts;
            else if (r < 0 && r != -EINVAL) ++errors;
            if (p.position() > p.length()) ++bad;
        }
        CHECK_EQ(p.stop(), 0);
        stop = true;
        reader.join();
        CHECK_EQ(timeouts, 0u);
        CHECK_EQ(errors, 0u);
        CHECK_EQ(bad.load(), 0u);
        CHECK(p.state() == PlayerState::Stopped);
        CHECK(sink.writes() > 0);
    }
    /* destroying a playing Player joins the thread and closes the sink */
    {
        const auto s = ramp_samples(48000 * 5, 1);
        const std::string path = d.file("kill.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        FakeSink sink;
        sink.sleep_us = 200;
        {
            Player p(posix_file_ops(), sink);
            CHECK_EQ(p.open(path), 0);
            CHECK_EQ(p.play(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(!sink.is_open());
    }
}
