/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests: the Bluetooth rig shared by test_bt.cpp (the input, task T2) and test_bt_out.cpp (playback and the monitor,
 * task T3): a controller with an Evolve 65 on FakePulse, a Feeder that plays the headset's capture side, and the helpers the
 * facts of the HackBerry probe (study 12) need. All audio is synthetic.
 */
#pragma once

#include "helpers.hpp"

#include "../fixture_codecs.hpp"
#include "bt_input.hpp"
#include "fake_pulse.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace tbt {

using namespace ta;

inline const std::string kMac = "70:BF:92:53:6B:8B";
inline const std::string kId = "bt:70:BF:92:53:6B:8B";
inline const std::string kVirt = "bluez_input.70:BF:92:53:6B:8B";
inline const std::string kReal = "bluez_input.70_BF_92_53_6B_8B.0";
inline const std::string kCard = "bluez_card.70_BF_92_53_6B_8B";
/* The music-mode virtual source: colon name, 48 kHz float32 mono, no card index. */
inline PulseDevice virtual_source() {
    PulseDevice d = fake_bt_source(kMac, 48000, kPulseNoIndex);
    d.name = kVirt;
    d.format = "float32le";
    d.bt_address.clear();
    d.bt_profile.clear();
    d.bt_codec.clear();
    d.form_factor.clear();
    d.bus.clear();
    d.device_api.clear();
    return d;
}

/* Delivers what the "server" captures into the newest open capture stream, 10 ms at a time, in real time. */
struct Feeder {
    enum Mode { Tone, Zeros, Off };
    FakePulse &f;
    std::atomic<int> mode{Tone};
    std::atomic<uint64_t> tone_frames{0}, zero_frames{0};
    std::atomic<bool> stop{false};
    std::thread th;
    explicit Feeder(FakePulse &p) : f(p) { th = std::thread([this] { run(); }); }
    ~Feeder() {
        stop = true;
        th.join();
    }
    void run() {
        uint64_t n = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop.load()) {
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
            const int m = mode.load();
            if (m == Off) continue;
            std::lock_guard<std::recursive_mutex> g(f.lock());
            size_t idx = static_cast<size_t>(-1);
            for (size_t i = f.streams.size(); i-- > 0;)
                if (f.streams[i].capture && f.streams[i].open && f.streams[i].cap) {
                    idx = i;
                    break;
                }
            if (idx == static_cast<size_t>(-1)) continue;
            const uint32_t rate = f.streams[idx].rate;
            const size_t frames = rate / 100;
            std::vector<int16_t> buf(frames);
            for (size_t i = 0; i < frames; ++i)
                buf[i] = m == Tone ? static_cast<int16_t>(std::lround(0.5 * 32767.0 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(n + i + 1) / rate)))
                                   : 0;
            if (f.deliver(idx, buf.data(), frames)) (m == Tone ? tone_frames : zero_frames) += frames;
            n += frames;
            f.advance(0.01);
        }
    }
};

/* A controller with a Bluetooth headset (Evolve 65) on the fake server; `usb` are ALSA cards. */
struct BtRig {
    Rig r;
    FakePulse pulse;
    std::unique_ptr<Feeder> feed;

    explicit BtRig(std::vector<FakeCard> usb = {}, const std::string &active = "a2dp-sink", bool with_headset = true)
        : r(std::move(usb)) {
        r.cfg.pulse = &pulse;
        r.cfg.server_running = [] { return true; };
        r.cfg.pulse_retry_ms = 100;
        r.cfg.pulse_refresh_ms = 20;
        r.cfg.writer_sync_ms = 300;
        pulse.server.default_source = "alsa_input.other";
        PulseDevice other = fake_usb_source("other");
        other.name = "alsa_input.other";
        pulse.sources.push_back(other);
        if (with_headset) add_headset(active);
        feed.reset(new Feeder(pulse));
    }
    /* the controller (its streams) goes first: its capture and playback streams close against the fake server */
    ~BtRig() { r.close(); }
    void add_headset(const std::string &active) {
        const bool call = active.rfind("headset", 0) == 0;
        std::lock_guard<std::recursive_mutex> g(pulse.lock());
        pulse.sources.push_back(call ? fake_bt_source(kMac, 16000, 70) : virtual_source());
        pulse.cards.push_back(fake_bt_card(kMac, 70, true, active));
        pulse.sinks.push_back(fake_bt_sink(kMac, 70));
    }
    int bt_index() {
        const Snapshot s = r.snap();
        for (size_t i = 0; i < s.card.inputs.size(); ++i)
            if (s.card.inputs[i].bluetooth) return static_cast<int>(i);
        return -1;
    }
    bool wait_inputs(size_t n, int ms = 4000) {
        return r.wait([n](const Snapshot &s) { return s.card.inputs.size() >= n; }, ms);
    }
    bool pick() {
        if (!wait_inputs(1)) return false;
        const int i = bt_index();
        return i >= 0 && r.ctl().select_card(i);
    }
    bool wait_present(int ms = 4000) { return r.wait([](const Snapshot &s) { return s.card.present; }, ms); }
    size_t streams() {
        std::lock_guard<std::recursive_mutex> g(pulse.lock());
        return pulse.streams.size();
    }
    bool stream_open(size_t i) {
        std::lock_guard<std::recursive_mutex> g(pulse.lock());
        return i < pulse.streams.size() && pulse.streams[i].open;
    }
};

/* ---- T3: playback and the monitor ---- */

/* The card follows the microphone like the HackBerry's Evolve 65 (P4): opening a capture stream on the headset switches the card to
 * the call profile after down_ms (its sink becomes 16 kHz mono); with no capture stream open it returns to a2dp-sink after up_ms. */
struct ProfileSim {
    FakePulse &f;
    int down_ms, up_ms;
    std::atomic<bool> stop{false};
    std::atomic<int> to_call{0}, to_music{0};
    std::thread th;
    ProfileSim(FakePulse &p, int down, int up) : f(p), down_ms(down), up_ms(up) { th = std::thread([this] { run(); }); }
    ~ProfileSim() {
        stop = true;
        th.join();
    }
    void run() {
        using clk = std::chrono::steady_clock;
        clk::time_point open_since{}, closed_since = clk::now();
        bool was_open = false;
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            bool open = false;
            std::string active;
            {
                std::lock_guard<std::recursive_mutex> g(f.lock());
                for (const FakeStream &s : f.streams)
                    if (s.capture && s.open && s.cap) open = true;
                if (!f.cards.empty()) active = f.cards[0].active_profile;
            }
            const auto now = clk::now();
            if (open && !was_open) open_since = now;
            if (!open && was_open) closed_since = now;
            was_open = open;
            if (active.empty()) continue;
            const bool call = active.rfind("headset", 0) == 0;
            if (open && !call && now - open_since >= std::chrono::milliseconds(down_ms)) {
                retarget("headset-head-unit", 16000, 1);
                ++to_call;
            } else if (!open && call && now - closed_since >= std::chrono::milliseconds(up_ms)) {
                retarget("a2dp-sink", 48000, 2);
                ++to_music;
            }
        }
    }
    void retarget(const char *profile, uint32_t rate, unsigned ch) {
        std::string card;
        {
            std::lock_guard<std::recursive_mutex> g(f.lock());
            for (PulseDevice &d : f.sinks)
                if (d.bt_address == kMac) {
                    d.rate = rate;
                    d.channels = ch;
                    d.bt_profile = profile;
                }
            card = f.cards[0].name;
        }
        f.set_profile(card, profile);
    }
};

/* The fake sound server's playback side in real time: every 10 ms each open playback stream plays rate/100 frames of what it holds
 * (short of audio after it started = an underflow), and keeps them. got(i) = what stream i played, including the end-of-take drain. */
struct PlaySrv {
    FakePulse &f;
    std::atomic<bool> stop{false};
    std::mutex m;
    std::vector<std::vector<int16_t>> data;
    std::vector<bool> started;
    std::thread th;
    explicit PlaySrv(FakePulse &p) : f(p) {
        th = std::thread([this] {
            auto next = std::chrono::steady_clock::now();
            while (!stop.load()) {
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
                std::lock_guard<std::recursive_mutex> g(f.lock());
                for (size_t i = 0; i < f.streams.size(); ++i) {
                    FakeStream &s = f.streams[i];
                    if (s.capture || !s.open || !s.play) continue;
                    const size_t want = s.rate / 100;
                    std::vector<int16_t> buf(want * s.channels);
                    const size_t got = s.play->take(buf.data(), want);
                    std::lock_guard<std::mutex> g2(m);
                    if (data.size() <= i) data.resize(i + 1), started.resize(i + 1, false);
                    if (got) started[i] = true;
                    data[i].insert(data[i].end(), buf.begin(), buf.begin() + static_cast<long>(got * s.channels));
                    if (started[i] && got < want) s.play->feed_underflow();
                }
            }
        });
    }
    ~PlaySrv() {
        stop = true;
        th.join();
    }
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
};

/* the index of the n-th playback (or capture) stream the fake server has seen, -1 when there is none */
inline int stream_index(FakePulse &f, bool capture, int nth = 0) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    for (size_t i = 0; i < f.streams.size(); ++i)
        if (f.streams[i].capture == capture && nth-- == 0) return static_cast<int>(i);
    return -1;
}
inline int count_streams(FakePulse &f, bool capture) {
    std::lock_guard<std::recursive_mutex> g(f.lock());
    int n = 0;
    for (const FakeStream &s : f.streams)
        if (s.capture == capture) ++n;
    return n;
}

}  // namespace tbt
