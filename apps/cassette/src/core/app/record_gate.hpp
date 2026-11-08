/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app: the gate between the ProcessingSink and the Ring. While no take runs it accepts and discards every frame
 * (the meter has already seen them), so the capture path can run all the time at no cost beyond the DSP. attach() opens it onto
 * a Ring; detach() closes it and waits until a write that was already in flight has returned, so the Ring may be destroyed
 * right after. Any thread may attach and detach; write() is the capture thread's and never blocks. LVGL-free.
 */
#pragma once

#include "../frame_sink.hpp"
#include "../ring.hpp"

#include <atomic>
#include <thread>

namespace cassette {

class RecordGate final : public FrameSink {
public:
    size_t write(const void *frames, size_t nframes) override {
        inflight_.fetch_add(1);
        Ring *r = target_.load();
        const size_t n = r ? r->write(frames, nframes) : nframes;
        inflight_.fetch_sub(1);
        return n;
    }
    void attach(Ring *r) { target_.store(r); }
    void detach() {
        target_.store(nullptr);
        while (inflight_.load() > 0) std::this_thread::yield();
    }
    bool attached() const { return target_.load() != nullptr; }

private:
    std::atomic<Ring *> target_{nullptr};
    std::atomic<int> inflight_{0};
};

}  // namespace cassette
