/* SPDX-License-Identifier: MIT */
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "capture.hpp"
#include "check.hpp"
#include "fake_alsa.hpp"

using namespace cassette;

namespace {

/* Keeps every byte it accepts, up to `limit` frames (then refuses), optionally taking only part of a write. */
struct TestSink : FrameSink {
    size_t bytes_per_frame = 2;
    size_t limit = ~size_t(0);
    size_t half = 0; /* when set, takes at most half of each write */
    std::mutex mu;
    std::vector<uint8_t> data;
    size_t frames = 0;
    size_t write(const void *p, size_t n) override {
        std::lock_guard<std::mutex> g(mu);
        size_t take = std::min(n, limit - std::min(limit, frames));
        if (half) take = std::min(take, (n + 1) / 2);
        const uint8_t *b = static_cast<const uint8_t *>(p);
        data.insert(data.end(), b, b + take * bytes_per_frame);
        frames += take;
        return take;
    }
    size_t count() {
        std::lock_guard<std::mutex> g(mu);
        return frames;
    }
};

struct Rig {
    FakeAlsa fake;
    TestSink sink;
    std::unique_ptr<Capture> cap;
    uint64_t period_frames_seen = 0;
    std::atomic<uint64_t> cb_frames{0};

    explicit Rig(FakeCard card) {
        fake.cards_ = {std::move(card)};
    }
    bool open(OpenOptions oo = OpenOptions()) {
        OpenResult r = open_capture(fake, fake.cards_[0].info, oo);
        if (!r.ok()) return false;
        sink.bytes_per_frame = static_cast<size_t>(r.format.channels) * (r.format.bits / 8);
        CaptureOptions co;
        co.now = [this] { return fake.clock; };
        co.on_period = [this](const void *, size_t n, const AudioFormat &) { cb_frames += n; };
        cap.reset(new Capture(std::move(r), sink, co));
        return true;
    }
};

/* S16 counter pattern: frame i is (i & 0x7fff) */
bool s16_pattern_ok(const std::vector<uint8_t> &d, size_t from_frame_count) {
    for (size_t i = 0; i < from_frame_count && 2 * i + 1 < d.size(); ++i) {
        uint32_t v = d[2 * i] | (static_cast<uint32_t>(d[2 * i + 1]) << 8);
        if (v != (i & 0x7fff)) return false;
    }
    return true;
}

void plain_capture_and_callback() {
    Rig rig(fake_pcm2902(0));
    CHECK(rig.open());
    CHECK(!rig.cap->via_server());
    CHECK(rig.cap->start());
    CHECK(!rig.cap->start()); /* already running */
    CHECK(wait_until([&] { return rig.sink.count() >= 20000; }, 3000));
    rig.cap->stop();
    CaptureStats s = rig.cap->stats();
    CHECK(!s.running);
    CHECK(s.frames_captured >= 20000);
    CHECK_EQ(s.frames_captured, rig.sink.count());
    CHECK_EQ(s.frames_dropped, 0u);
    CHECK_EQ(s.overruns, 0u);
    CHECK(s.last_error == CaptureError::None);
    CHECK(s.stop_reason == CaptureError::None);
    CHECK_EQ(rig.cb_frames.load(), s.frames_captured); /* the level meter saw every period */
    CHECK(s16_pattern_ok(rig.sink.data, s.frames_captured));
}

void short_reads_keep_the_stream_intact() {
    FakeCard c = fake_pcm2902(0);
    c.short_read_every = 3;
    Rig rig(c);
    CHECK(rig.open());
    CHECK(rig.cap->start());
    CHECK(wait_until([&] { return rig.sink.count() >= 10000; }, 3000));
    rig.cap->stop();
    CHECK(s16_pattern_ok(rig.sink.data, rig.sink.count()));
    CHECK_EQ(rig.cap->stats().frames_dropped, 0u);
}

void sample_formats() {
    /* S24_3LE kept as it is */
    FakeCard c = fake_pcm2902(0);
    c.offer.formats = {SampleFormat::S24_3LE};
    Rig a(c);
    CHECK(a.open());
    CHECK_EQ(a.sink.bytes_per_frame, 3u);
    CHECK(a.cap->start());
    CHECK(wait_until([&] { return a.sink.count() >= 3000; }, 3000));
    a.cap->stop();
    bool ok = true;
    for (size_t i = 0; i < 3000; ++i) {
        uint32_t v = a.sink.data[3 * i] | (a.sink.data[3 * i + 1] << 8) | (a.sink.data[3 * i + 2] << 16);
        if (v != (i & 0xffffff)) ok = false;
    }
    CHECK(ok);

    /* S32 converted to packed 24-bit: the fake puts 0xAA in the low byte, which must be gone */
    c.offer.formats = {SampleFormat::S32_LE};
    Rig b(c);
    CHECK(b.open());
    CHECK_EQ(b.sink.bytes_per_frame, 3u);
    CHECK(b.cap->start());
    CHECK(wait_until([&] { return b.sink.count() >= 3000; }, 3000));
    b.cap->stop();
    ok = true;
    for (size_t i = 0; i < 3000; ++i) {
        uint32_t v = b.sink.data[3 * i] | (b.sink.data[3 * i + 1] << 8) | (b.sink.data[3 * i + 2] << 16);
        if (v != (i & 0xffffff)) ok = false;
    }
    CHECK(ok);
}

void xrun_is_counted_and_estimated() {
    FakeCard c = fake_pcm2902(0);
    c.events = {{FakeEvent::Xrun, 4096, 0, 0.25}, {FakeEvent::Xrun, 9216, 0, 0.5}};
    Rig rig(c);
    CHECK(rig.open());
    CHECK(rig.cap->start());
    CHECK(wait_until([&] { return rig.cap->stats().overruns == 2 && rig.sink.count() >= 14000; }, 3000));
    rig.cap->stop();
    CaptureStats s = rig.cap->stats();
    CHECK_EQ(s.overruns, 2u);
    /* gaps 0.25 s + 0.5 s at 48000 Hz = 36000 frames (an estimate from the clock) */
    CHECK(s.frames_lost_xrun >= 35990 && s.frames_lost_xrun <= 36010);
    CHECK(s.last_error == CaptureError::Overrun);
    CHECK(s.stop_reason == CaptureError::None); /* an overrun does not stop the capture */
    CHECK(s.message.find("lost") != std::string::npos);
    CHECK(s.frames_captured >= 14000);
}

void unplug_stops_cleanly(int err, const char *what) {
    FakeCard c = fake_uc02(0);
    c.events = {{FakeEvent::Error, 3000, err, 0}};
    Rig rig(c);
    CHECK(rig.open());
    CHECK(rig.cap->start());
    bool ended = wait_until([&] { return !rig.cap->running(); }, 2000); /* it stops by itself, in a bounded time */
    CHECK(ended);
    CaptureStats s = rig.cap->stats();
    CHECK(s.stop_reason == CaptureError::Unplugged);
    CHECK(s.last_error == CaptureError::Unplugged);
    CHECK_EQ(s.last_errno, err);
    CHECK_EQ(s.frames_captured, 3000u);
    CHECK_EQ(rig.fake.reads_after_error, 0u); /* no busy loop: not one read after the error */
    CHECK(s.message.find("disconnected") != std::string::npos);
    CHECK(!rig.cap->start()); /* the PCM was closed: open again through open_capture */
    rig.cap->stop();          /* safe after it ended by itself, twice */
    rig.cap->stop();
    (void)what;
}

void unplug_in_both_forms() {
    unplug_stops_cleanly(-ENODEV, "ENODEV");
    unplug_stops_cleanly(-EBADFD, "EBADFD"); /* "File descriptor in bad state": arecord at the P0 unplug */
    CHECK(classify_error(-ENODEV) == CaptureError::Unplugged);
    CHECK(classify_error(-EBADFD) == CaptureError::Unplugged);
    CHECK(classify_error(-EPIPE) == CaptureError::Overrun);
    CHECK(classify_error(-EBUSY) == CaptureError::Busy);
    CHECK(classify_error(-EIO) == CaptureError::Other);
    CHECK(classify_error(0) == CaptureError::None);
    CHECK_EQ(std::string(capture_error_name(CaptureError::Unplugged)), "unplugged");
}

void busy_and_other_errors() {
    FakeCard c = fake_pcm2902(0);
    c.events = {{FakeEvent::Error, 2048, -EBUSY, 0}};
    Rig a(c);
    CHECK(a.open());
    CHECK(a.cap->start());
    CHECK(wait_until([&] { return !a.cap->running(); }, 2000));
    CHECK(a.cap->stats().stop_reason == CaptureError::Busy);
    CHECK_EQ(a.fake.reads_after_error, 0u);

    c.events = {{FakeEvent::Error, 2048, -EIO, 0}};
    Rig b(c);
    CHECK(b.open());
    CHECK(b.cap->start());
    CHECK(wait_until([&] { return !b.cap->running(); }, 2000));
    CHECK(b.cap->stats().stop_reason == CaptureError::Other);
    CHECK_EQ(b.cap->stats().last_errno, -EIO);
    CHECK(b.fake.reads_after_error <= 1);
}

void sink_refusal_counts_drops() {
    Rig rig(fake_pcm2902(0));
    rig.sink.limit = 5000; /* a full ring */
    CHECK(rig.open());
    CHECK(rig.cap->start());
    CHECK(wait_until([&] { return rig.cap->stats().frames_captured >= 12000; }, 3000));
    rig.cap->stop();
    CaptureStats s = rig.cap->stats();
    CHECK_EQ(rig.sink.count(), 5000u);
    CHECK_EQ(s.frames_dropped, s.frames_captured - 5000);
    CHECK(s.frames_dropped >= 7000);
    CHECK_EQ(rig.cb_frames.load(), s.frames_captured); /* the meter still sees refused frames */

    /* partial acceptance */
    Rig part(fake_pcm2902(0));
    part.sink.half = 1;
    CHECK(part.open());
    CHECK(part.cap->start());
    CHECK(wait_until([&] { return part.cap->stats().frames_captured >= 6000; }, 3000));
    part.cap->stop();
    CaptureStats p = part.cap->stats();
    CHECK_EQ(p.frames_dropped + part.sink.count(), p.frames_captured);
    CHECK(p.frames_dropped > 0);
}

void start_stop_twice() {
    Rig rig(fake_pcm2902(0));
    CHECK(rig.open());
    CHECK(rig.cap->start());
    CHECK(wait_until([&] { return rig.sink.count() >= 3000; }, 3000));
    rig.cap->stop();
    rig.cap->stop(); /* twice is fine */
    uint64_t c1 = rig.cap->stats().frames_captured;
    CHECK(c1 >= 3000);
    CHECK(rig.cap->start()); /* and again */
    CHECK(wait_until([&] { return rig.cap->stats().frames_captured >= c1 + 3000; }, 3000));
    rig.cap->stop();
    CHECK(!rig.cap->running());
    CHECK(rig.cap->stats().frames_captured >= c1 + 3000);
    /* stop without start, destructor while running */
    Rig idle(fake_pcm2902(0));
    CHECK(idle.open());
    idle.cap->stop();
    Rig busy(fake_pcm2902(0));
    CHECK(busy.open());
    CHECK(busy.cap->start());
    busy.cap.reset(); /* ~Capture joins */
    CHECK(true);
}

void stuck_card_does_not_spin() {
    FakeCard c = fake_pcm2902(0);
    c.zero_reads = true;
    Rig rig(c);
    CHECK(rig.open());
    CHECK(rig.cap->start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.cap->stop();
    CHECK_EQ(rig.cap->stats().frames_captured, 0u);
}

void repeated_start_stop() {
    /* one Capture, 200 start/stop cycles with random short runs */
    std::srand(7);
    Rig rig(fake_pcm2902(0));
    CHECK(rig.open());
    bool all_ok = true;
    for (int i = 0; i < 200; ++i) {
        if (!rig.cap->start()) all_ok = false;
        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 1500));
        rig.cap->stop();
        if (rig.cap->running()) all_ok = false;
    }
    CHECK(all_ok);
    CHECK(rig.cap->stats().frames_captured > 0);
    CHECK_EQ(rig.cap->stats().frames_captured, rig.sink.count());

    /* 200 fresh captures that die mid-run, one way or another */
    bool fine = true;
    for (int i = 0; i < 200; ++i) {
        FakeCard c = fake_uc02(0);
        if (i % 3 == 0) c.events = {{FakeEvent::Error, 500, -EBADFD, 0}};
        else if (i % 3 == 1) c.events = {{FakeEvent::Xrun, 500, 0, 0.1}};
        Rig r(c);
        if (!r.open() || !r.cap->start()) {
            fine = false;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 1500));
        if (i % 2) r.cap->stop();
        /* else: the destructor stops it */
    }
    CHECK(fine);
}

} // namespace

void test_capture() {
    plain_capture_and_callback();
    short_reads_keep_the_stream_intact();
    sample_formats();
    xrun_is_counted_and_estimated();
    unplug_in_both_forms();
    busy_and_other_errors();
    sink_refusal_counts_drops();
    start_stop_twice();
    stuck_card_does_not_spin();
    repeated_start_stop();
}
