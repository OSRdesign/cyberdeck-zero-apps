/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the fake sound output (see fake_output.hpp).
 */
#include "play/fake_output.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>

namespace vhs {

FakeOutputOptions fake_output_options(const std::string &mode) {
    FakeOutputOptions o;
    if (mode == "lost") o.lost_after_ms = 1500;
    else if (mode == "none") o.open_error = "no device";
    else if (mode == "instant") o.pace = false;
    else if (mode == "steppy") o.step_frames = 2048;
    else if (mode == "dummy") {
        o.label = "Dummy Output";
        o.name = "auto_null";
    }
    return o;
}

namespace {

using Clock = std::chrono::steady_clock;

class FakeOutSink final : public PlaybackSink {
public:
    explicit FakeOutSink(const FakeOutputOptions &o) : o_(o) {}
    int open(const AudioFormat &src, SinkFormat &g) override {
        g.rate = src.rate;
        g.channels = src.channels;
        g.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        g.period_frames = 1024;
        g.buffer_frames = src.rate / 2;
        rate_ = src.rate ? src.rate : 48000;
        restart();
        opened_ = Clock::now();
        return 0;
    }
    long write(const void *, size_t n) override {
        if (o_.lost_after_ms > 0 && Clock::now() - opened_ > std::chrono::milliseconds(o_.lost_after_ms)) return -ENODEV;
        if (o_.pace) { /* the buffer holds 50 ms: wait until the device has played enough */
            while (static_cast<double>(written_) > heard() + rate_ * 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        written_ += n;
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override {
        if (!o_.pace) return 0;
        double h = heard();
        if (o_.step_frames > 0) h = static_cast<double>(static_cast<long long>(h) / o_.step_frames * o_.step_frames);
        return static_cast<double>(written_) > h ? static_cast<long>(static_cast<double>(written_) - h) : 0;
    }
    void drop() override { restart(); }
    void drain() override { written_ = 0; start_ = Clock::now(); }
    void close() override {}

private:
    void restart() {
        written_ = 0;
        start_ = Clock::now();
    }
    double heard() const { return std::chrono::duration<double>(Clock::now() - start_).count() * rate_; }
    FakeOutputOptions o_;
    uint32_t rate_ = 48000;
    uint64_t written_ = 0;
    Clock::time_point start_ = Clock::now(), opened_ = Clock::now();
};

class FakeOutProvider final : public SinkProvider {
public:
    explicit FakeOutProvider(const FakeOutputOptions &o) : o_(o) {}
    int open(const OutputTarget &t, OpenedSink &out, std::string &why) override {
        if (!o_.open_error.empty()) {
            why = o_.open_error;
            return -ENODEV;
        }
        out.sink = std::make_unique<FakeOutSink>(o_);
        out.name = !o_.name.empty() ? o_.name : (t.kind == OutputTarget::Kind::Auto ? "default" : t.id);
        out.label = o_.label;
        out.device = out.name;
        out.pulse = false;
        return 0;
    }
    bool still_there(const OpenedSink &) override { return o_.lost_after_ms <= 0; }

private:
    FakeOutputOptions o_;
};

}  // namespace

std::unique_ptr<AudioPlayerDeps> fake_audio_deps(const FakeOutputOptions &o) {
    auto d = std::make_unique<AudioPlayerDeps>();
    d->sinks = std::make_unique<FakeOutProvider>(o);
    return d;
}

}  // namespace vhs
