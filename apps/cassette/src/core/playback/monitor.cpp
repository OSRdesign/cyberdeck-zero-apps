/* SPDX-License-Identifier: MIT */
#include "monitor.hpp"

#include "../dsp/pcm.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace cassette {

float monitor_gain(int pct) {
    const int p = std::clamp(pct, 0, 100);
    const float x = static_cast<float>(p) / 100.0f;
    return x * x;
}

int monitor_level_clamp(int pct) {
    const int p = std::clamp(pct, 0, 100);
    return std::clamp(((p + 2) / 5) * 5, 0, 100);
}

MonitorTap::MonitorTap(const AudioFormat &fmt, double seconds) : fmt_(fmt), ring_(std::make_shared<Ring>(fmt, seconds)) {}

size_t MonitorTap::write(const void *frames, size_t nframes) {
    if (frames && active_.load(std::memory_order_acquire)) ring_->write(frames, nframes); /* a full ring drops */
    return nframes;
}

struct MonitorShared {
    std::unique_ptr<PlaybackSink> sink;
    std::shared_ptr<Ring> ring;
    AudioFormat src;
    SinkFormat out;
    std::atomic<bool> quit{false}, running{false}, done{true}, abandoned{false};
    std::atomic<int> level{kMonitorLevelDefault}, error{0};
    std::atomic<uint64_t> played{0}, underruns{0}, resyncs{0};
    ~MonitorShared() {
        if (sink) sink->close();
    }
};

namespace {

using Clock = std::chrono::steady_clock;

float sample_in(const uint8_t *p, uint16_t bits) { return static_cast<float>(pcm::read(p, bits)) / pcm::scale(bits); }

void sample_out(uint8_t *p, SampleFormat f, float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    switch (f) {
    case SampleFormat::S24_3LE: {
        const int32_t s = static_cast<int32_t>(std::lround(std::clamp(v * 8388608.0f, -8388608.0f, 8388607.0f)));
        pcm::write(p, 24, s);
        break;
    }
    case SampleFormat::S32_LE: {
        const double d = std::clamp(static_cast<double>(v) * 2147483648.0, -2147483648.0, 2147483647.0);
        const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(d));
        p[0] = uint8_t(u);
        p[1] = uint8_t(u >> 8);
        p[2] = uint8_t(u >> 16);
        p[3] = uint8_t(u >> 24);
        break;
    }
    default: {
        const int32_t s = static_cast<int32_t>(std::lround(std::clamp(v * 32768.0f, -32768.0f, 32767.0f)));
        pcm::write(p, 16, s);
        break;
    }
    }
}

/* The monitor thread. All buffers are allocated here, before the loop: the loop never allocates. */
void run(std::shared_ptr<MonitorShared> sh) {
    MonitorShared &s = *sh;
    const unsigned in_ch = s.src.channels, out_ch = s.out.channels;
    const size_t in_bps = pcm::bytes_per_sample(s.src.bits);
    const size_t in_fb = in_bps * in_ch;
    const size_t out_bps = format_bytes(s.out.format);
    const size_t out_fb = out_bps * out_ch;
    const size_t chunk =
        std::clamp<size_t>(s.out.period_frames ? s.out.period_frames : kMonitorPeriodFrames, 64, kMonitorPeriodFrames);
    const size_t max_backlog = static_cast<size_t>(kMonitorMaxBacklogMs * s.src.rate / 1000.0);
    const long max_queue = static_cast<long>(kMonitorMaxQueueMs * s.src.rate / 1000.0);
    std::vector<uint8_t> in(chunk * in_fb), out(chunk * out_fb);
    std::vector<float> fr(in_ch);
    float cur = monitor_gain(s.level.load(std::memory_order_relaxed));
    bool failed = false;
    /* a little silence in front: the capture delivers 1024 frames at a time, so the device must hold some margin or every
     * bit of jitter would be an underrun; the same after a recovered underrun */
    std::vector<uint8_t> hush(static_cast<size_t>(kMonitorPrefillFrames) * out_fb, 0);
    auto prefill = [&s, &hush, out_fb]() {
        size_t left = kMonitorPrefillFrames;
        while (left > 0 && !s.quit.load(std::memory_order_acquire)) {
            const long w = s.sink->write(hush.data() + (kMonitorPrefillFrames - left) * out_fb, left);
            if (w <= 0) return;
            left -= static_cast<size_t>(w);
        }
    };
    prefill();

    while (!failed && !s.quit.load(std::memory_order_acquire)) {
        const size_t avail = s.ring->available();
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (avail > max_backlog) { /* behind: drop the oldest audio, keep the newest chunk */
            size_t skip = avail - chunk;
            while (skip > 0) {
                const size_t k = s.ring->read(in.data(), std::min(skip, chunk));
                if (k == 0) break;
                skip -= k;
            }
            s.resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        const size_t n = s.ring->read(in.data(), chunk);
        if (n == 0) continue;
        const float target = monitor_gain(s.level.load(std::memory_order_relaxed));
        const float step = (target - cur) / static_cast<float>(n);
        float g = cur;
        for (size_t i = 0; i < n; ++i) {
            g += step;
            const uint8_t *p = in.data() + i * in_fb;
            for (unsigned c = 0; c < in_ch; ++c) fr[c] = sample_in(p + c * in_bps, s.src.bits) * g;
            uint8_t *o = out.data() + i * out_fb;
            for (unsigned c = 0; c < out_ch; ++c) {
                float v;
                if (out_ch == in_ch) {
                    v = fr[c];
                } else if (out_ch == 1) {
                    v = 0;
                    for (unsigned k = 0; k < in_ch; ++k) v += fr[k];
                    v /= static_cast<float>(in_ch);
                } else {
                    v = fr[c < in_ch ? c : in_ch - 1]; /* mono to stereo: the channel twice */
                }
                sample_out(o + c * out_bps, s.out.format, v);
            }
        }
        cur = target;
        if (s.sink->delay() > max_queue) { /* the device holds too much: what it holds is late, throw it away */
            s.sink->drop();
            s.resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        size_t done = 0;
        while (done < n && !s.quit.load(std::memory_order_acquire)) {
            const long w = s.sink->write(out.data() + done * out_fb, n - done);
            if (w > 0) {
                done += static_cast<size_t>(w);
                s.played.fetch_add(static_cast<uint64_t>(w), std::memory_order_relaxed);
            } else if (w == -EPIPE || w == -ESTRPIPE) {
                const int r = s.sink->recover(static_cast<int>(w));
                if (r < 0) {
                    s.error.store(r);
                    failed = true;
                    break;
                }
                s.underruns.fetch_add(1, std::memory_order_relaxed); /* resynced: the delay starts from zero again */
                prefill();
            } else if (w == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                s.error.store(static_cast<int>(w));
                failed = true;
                break;
            }
        }
    }
    s.running.store(false, std::memory_order_release);
    s.sink->close();
    s.done.store(true, std::memory_order_release);
}

} // namespace

Monitor::Monitor(MonitorTap &tap) : tap_(tap), sh_(std::make_shared<MonitorShared>()) {}

Monitor::~Monitor() { stop(); }

int Monitor::start(std::unique_ptr<PlaybackSink> sink) {
    if (!sink) return -EINVAL;
    stop(); /* an earlier run: reaped (or abandoned) */
    auto sh = std::make_shared<MonitorShared>();
    sh->level.store(sh_->level.load());
    sh->src = tap_.format();
    sh->ring = tap_.ring();
    if (!pcm::supported(sh->src.bits) || sh->src.channels == 0) return -EINVAL;
    sh->sink = std::move(sink);
    sh->sink->set_buffering(kMonitorPeriodFrames, kMonitorBufferFrames);
    SinkFormat granted;
    const int r = sh->sink->open(sh->src, granted);
    if (r < 0) {
        sh->sink->close();
        return r;
    }
    if (granted.channels == 0 || (granted.rate != 0 && granted.rate != sh->src.rate)) {
        sh->sink->close();
        return -EINVAL;
    }
    sh->out = granted;
    if (sh->out.period_frames == 0) sh->out.period_frames = kMonitorPeriodFrames;
    { /* what the ring holds from before the start is old: drop it (no consumer runs now, the tap is not active yet) */
        std::vector<uint8_t> scratch(4096 * pcm::bytes_per_frame(sh->src));
        while (sh->ring->read(scratch.data(), 4096) > 0) {
        }
    }
    sh->running.store(true);
    sh->done.store(false);
    sh_ = sh;
    tap_.set_active(true);
    std::thread(run, sh).detach(); /* the thread owns a share of the state; stop() waits for `done` */
    return 0;
}

void Monitor::stop() {
    tap_.set_active(false);
    MonitorShared &s = *sh_;
    s.quit.store(true, std::memory_order_release);
    if (s.done.load(std::memory_order_acquire)) return;
    const auto end = Clock::now() + std::chrono::milliseconds(kMonitorStopWaitMs);
    while (!s.done.load(std::memory_order_acquire) && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!s.done.load(std::memory_order_acquire)) s.abandoned.store(true); /* a write that does not return: it cleans up itself */
}

bool Monitor::running() const { return sh_->running.load(std::memory_order_acquire); }

void Monitor::set_level_pct(int pct) { sh_->level.store(std::clamp(pct, 0, 100), std::memory_order_relaxed); }

int Monitor::level_pct() const { return sh_->level.load(std::memory_order_relaxed); }

MonitorStats Monitor::stats() const {
    MonitorStats st;
    const MonitorShared &s = *sh_;
    st.running = s.running.load();
    st.error = s.error.load();
    st.frames_played = s.played.load();
    st.underruns = s.underruns.load();
    st.resyncs = s.resyncs.load();
    st.frames_dropped_in = tap_.dropped();
    st.sink_period = s.out.period_frames;
    st.sink_buffer = s.out.buffer_frames;
    st.sink_rate = s.out.rate;
    st.sink_channels = s.out.channels;
    st.latency_ms = s.out.rate ? 1000.0 * s.out.buffer_frames / s.out.rate : 0.0;
    st.stalled_abandoned = s.abandoned.load();
    return st;
}

} // namespace cassette
