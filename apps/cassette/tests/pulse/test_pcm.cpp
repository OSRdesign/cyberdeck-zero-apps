/*
 * SPDX-License-Identifier: MIT
 *
 * A capture stream as an AlsaPcm: the existing capture thread (alsa/capture.*) runs on it unchanged. Frames flow into the
 * sink, the owner polls the stream's events on its own thread, and a device that goes away stops the thread with
 * CaptureError::Unplugged (the controller's DeviceLost path) after the last frames were delivered.
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_pcm.hpp"

#include "capture.hpp"

#include <atomic>
#include <cerrno>
#include <vector>

using namespace cassette;

namespace {

const std::string kEvolve = "70:BF:92:53:6B:8B";

struct CountSink : FrameSink {
    std::atomic<size_t> n{0};
    std::atomic<bool> pattern_ok{true};
    std::atomic<int16_t> next{1};
    size_t write(const void *frames, size_t k) override {
        const int16_t *s = static_cast<const int16_t *>(frames);
        int16_t want = next.load();
        for (size_t i = 0; i < k; ++i) {
            if (s[i] != want) pattern_ok = false;
            want = static_cast<int16_t>(want + 1);
        }
        next = want;
        n += k;
        return k;
    }
};

void test_capture_thread_on_pulse() {
    CHECK_EQ(pulse_end_errno(kPulseKilled), -ENODEV);
    CHECK_EQ(pulse_end_errno(kPulseNoDevice), -ENODEV);
    CHECK_EQ(pulse_end_errno(kPulseMoved), -ENODEV);
    CHECK_EQ(pulse_end_errno(kPulseDisconnected), -ENODEV);
    CHECK_EQ(pulse_end_errno(kPulseFailed), -EIO);
    CHECK(classify_error(pulse_end_errno(kPulseKilled)) == CaptureError::Unplugged);
    CHECK(classify_error(pulse_end_errno(kPulseFailed)) == CaptureError::Other);

    FakePulse f;
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.connect("Cassette", 1000);
    PulseCaptureRequest r;
    r.source = fake_bt_source(kEvolve).name;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    if (!cap) return;
    std::shared_ptr<PulseCapture> events(std::move(cap)); /* the owner's share, for the events */
    OpenResult o = pulse_open_result(events, 20);
    CHECK(o.ok());
    CHECK(o.via_server);
    CHECK_EQ(o.device, r.source);
    CHECK_EQ(o.format.rate, 16000u);
    CHECK_EQ(o.format.channels, 1u);
    CHECK_EQ(o.format.bits, 16u);
    CHECK_EQ(o.hw.rate, 16000u);
    CHECK(o.hw.format == SampleFormat::S16_LE);
    CHECK_EQ(o.hw.period_frames, 320u); /* the 20 ms fragment */
    CHECK_EQ(o.hw.buffer_frames, 32000u); /* the 2 s ring */
    CHECK(o.conv == Conversion::None);
    CHECK(o.pcm != nullptr);

    CountSink sink;
    CaptureOptions copt;
    copt.period_frames = 320;
    std::atomic<size_t> periods{0};
    copt.on_period = [&](const void *, size_t, const AudioFormat &fmt) {
        if (fmt.rate == 16000) ++periods;
    };
    Capture c(std::move(o), sink, copt);
    CHECK(c.via_server());
    CHECK(c.start());
    std::vector<int16_t> chunk(160);
    int16_t k = 1;
    for (int i = 0; i < 10; ++i) {
        for (auto &s : chunk) s = k++;
        f.deliver(0, chunk.data(), chunk.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(wait_until([&] { return sink.n.load() == 1600; }, 2000));
    CHECK(sink.pattern_ok.load());
    CHECK(periods.load() > 0);
    PulseStreamEvent ev;
    CHECK(events->poll_event(ev) && ev.kind == PulseEvent::Connecting);
    CHECK(events->poll_event(ev) && ev.kind == PulseEvent::Running);
    CHECK(events->source_identity_ok());
    CHECK(c.running());

    for (auto &s : chunk) s = k++;
    f.deliver(0, chunk.data(), chunk.size()); /* the last frames before the headset goes away */
    f.remove_source(r.source);
    CHECK(wait_until([&] { return !c.running(); }, 2000));
    const CaptureStats st = c.stats();
    CHECK(st.stop_reason == CaptureError::Unplugged);
    CHECK_EQ(st.frames_captured, 1760u); /* nothing lost at the end */
    CHECK_EQ(sink.n.load(), 1760u);
    CHECK(sink.pattern_ok.load());
    CHECK(events->poll_event(ev) && ev.kind == PulseEvent::Killed); /* the Capture dropped its PCM; our share lives */
    CHECK(events->ended());
    CHECK(!events->source_identity_ok());
    CHECK_EQ(events.use_count(), 1L);
    CHECK(!c.start()); /* the PCM is closed after a lost device: the owner opens a new stream */
}

void test_stop_while_connecting() {
    FakePulse f;
    f.sources.push_back(fake_bt_source(kEvolve, 16000, 70));
    f.connect("Cassette", 1000);
    PulseCaptureRequest r;
    r.source = fake_bt_source(kEvolve).name;
    std::unique_ptr<PulseCapture> cap;
    CHECK_EQ(f.open_capture(r, cap, nullptr), 0);
    PulseCapturePcm *pcm = new PulseCapturePcm(std::move(cap), 50);
    CHECK_EQ(pcm->prepare(), 0);
    std::vector<int16_t> buf(64);
    CHECK_EQ(pcm->readi(buf.data(), 64), 0L); /* no frame yet: 0, the thread asks again */
    OpenResult o;
    o.status = OpenStatus::Ok;
    o.via_server = true;
    o.format.rate = 16000;
    o.format.channels = 1;
    o.hw.rate = 16000;
    o.hw.channels = 1;
    o.pcm.reset(pcm);
    CountSink sink;
    Capture c(std::move(o), sink);
    CHECK(c.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto t0 = std::chrono::steady_clock::now();
    c.stop(); /* the thread is waiting inside readi: it ends within one read timeout */
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK(!c.running());
    CHECK(c.stats().stop_reason == CaptureError::None);
    CHECK(c.start()); /* stopped, not lost: starts again */
    c.stop();

    OpenResult none = pulse_open_result(nullptr);
    CHECK(!none.ok());
    CHECK(none.status == OpenStatus::NoCard);
    PulseCapturePcm empty(nullptr);
    CHECK_EQ(empty.readi(buf.data(), 1), static_cast<long>(-ENODEV));
    CHECK_EQ(empty.prepare(), -ENODEV);
    CHECK_EQ(empty.recover(-EPIPE), -EPIPE);
}

}  // namespace

void test_pcm() {
    test_capture_thread_on_pulse();
    test_stop_while_connecting();
}
