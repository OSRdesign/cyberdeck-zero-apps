/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette app tests (task T6a): a rig around the Controller on fakes. A fake sound system (tests/alsa) that delivers a
 * generated tone, a thread-safe wrapper for it (plug and unplug while the controller runs), a clock whose time can be
 * pushed forward, a fault-injecting FileOps (ENOSPC, free space, slow writes, a failing peak-cache rename) and a recording
 * playback sink (tests/playback). All audio is synthetic; no recording is ever used.
 */
#pragma once

#include "check.hpp"
#include "controller.hpp"
#include "fake_alsa.hpp"
#include "fileops.hpp"
#include "journal.hpp"
#include "sink.hpp"
#include "support.hpp"
#include "wav.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ta {

using namespace cassette;

constexpr double kPi = 3.14159265358979323846;

/* A clock that runs in real time and can be pushed forward (the record countdown and the Writer see it). Thread-safe. */
class TestClock final : public Clock {
public:
    std::atomic<uint64_t> skew{0};
    uint64_t mono_ms() override {
        const auto n = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(n).count()) + skew.load();
    }
    int64_t wall_s() override {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
    }
    void sleep_ms(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    void advance(uint64_t ms) { skew.fetch_add(ms); }
};

class SetClockState final : public ClockState {
public:
    bool synchronized() override { return true; }
};

/* The capture PCM of the wrapper: whatever the fake delivers is replaced by a tone; unplug() makes it fail with -ENODEV. */
class TonePcm final : public AlsaPcm {
public:
    TonePcm(std::unique_ptr<AlsaPcm> inner, const std::atomic<bool> *unplug, const HwGranted &hw, double hz, double amp)
        : inner_(std::move(inner)), unplug_(unplug), hw_(hw), hz_(hz), amp_(amp) {}
    long readi(void *buf, size_t frames) override {
        if (unplug_->load()) return -ENODEV;
        const long n = inner_->readi(buf, frames);
        if (n > 0 && hw_.format == SampleFormat::S16_LE) {
            int16_t *p = static_cast<int16_t *>(buf);
            for (long i = 0; i < n; ++i) {
                const double v = amp_ * 32767.0 * std::sin(2.0 * kPi * hz_ * static_cast<double>(done_ + static_cast<uint64_t>(i)) / hw_.rate);
                for (unsigned c = 0; c < hw_.channels; ++c) *p++ = static_cast<int16_t>(std::lround(v));
            }
            done_ += static_cast<uint64_t>(n);
        } else if (n > 0 && hw_.format == SampleFormat::S24_3LE) { /* packed 24-bit: the same tone, three bytes a sample */
            uint8_t *p = static_cast<uint8_t *>(buf);
            for (long i = 0; i < n; ++i) {
                const double v = amp_ * 8388607.0 * std::sin(2.0 * kPi * hz_ * static_cast<double>(done_ + static_cast<uint64_t>(i)) / hw_.rate);
                const int32_t q = static_cast<int32_t>(std::lround(v));
                for (unsigned c = 0; c < hw_.channels; ++c) {
                    *p++ = static_cast<uint8_t>(q & 0xFF);
                    *p++ = static_cast<uint8_t>((q >> 8) & 0xFF);
                    *p++ = static_cast<uint8_t>((q >> 16) & 0xFF);
                }
            }
            done_ += static_cast<uint64_t>(n);
        }
        return n;
    }
    int recover(int err) override { return inner_->recover(err); }
    int prepare() override { return inner_->prepare(); }
    void close() override { inner_->close(); }

private:
    std::unique_ptr<AlsaPcm> inner_;
    const std::atomic<bool> *unplug_;
    HwGranted hw_;
    double hz_, amp_;
    uint64_t done_ = 0;
};

/* FakeAlsa made safe for a controller with threads, delivering a 440 Hz tone at half scale. */
class ToneAlsa final : public FakeAlsa {
public:
    std::mutex m;
    std::atomic<bool> unplugged{false};
    double hz = 440.0, amp = 0.5;

    template <typename F> void with(F f) {
        std::lock_guard<std::mutex> g(m);
        f(*this);
    }
    void unplug(const std::string &id) {
        std::lock_guard<std::mutex> g(m);
        unplugged.store(true);
        for (FakeCard &c : cards_)
            if (c.info.id == id) c.present = false;
    }
    void replug(const std::string &id, bool reboot = true) {
        std::lock_guard<std::mutex> g(m);
        unplugged.store(false);
        for (FakeCard &c : cards_)
            if (c.info.id == id) {
                c.present = true;
                if (reboot) /* the mixer starts from its defaults again */
                    for (auto &kv : c.values) kv.second = kv.first.find("Switch") != std::string::npos ? 1 : 0;
            }
    }
    long mixer_value(const std::string &id, const std::string &name) {
        std::lock_guard<std::mutex> g(m);
        FakeCard *c = find_id(id);
        if (!c) return -999;
        auto it = c->values.find(name);
        return it == c->values.end() ? -999 : it->second;
    }

    bool available(std::string *why) override { return FakeAlsa::available(why); }
    std::vector<CardInfo> cards() override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::cards();
    }
    int query_capture(const std::string &d, CaptureOffer &o) override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::query_capture(d, o);
    }
    int open_capture(const std::string &d, const HwRequest &r, std::unique_ptr<AlsaPcm> &pcm, HwGranted &g) override {
        std::lock_guard<std::mutex> lk(m);
        const int e = FakeAlsa::open_capture(d, r, pcm, g);
        if (e == 0) pcm.reset(new TonePcm(std::move(pcm), &unplugged, g, hz, amp));
        return e;
    }
    int mixer_list(const std::string &c, std::vector<CtlInfo> &o) override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::mixer_list(c, o);
    }
    int mixer_get(const std::string &c, const std::string &n, long &v) override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::mixer_get(c, n, v);
    }
    int mixer_set(const std::string &c, const std::string &n, long v) override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::mixer_set(c, n, v);
    }
    int query_playback(const std::string &d, PlaybackOffer &o) override {
        std::lock_guard<std::mutex> g(m);
        return FakeAlsa::query_playback(d, o);
    }
    int open_playback(const std::string &d, const HwRequest &r, std::unique_ptr<AlsaPcm> &p, HwGranted &g) override {
        std::lock_guard<std::mutex> lk(m);
        return FakeAlsa::open_playback(d, r, p, g);
    }
};

/* The real file system with switches: ENOSPC on take files after N bytes, an overridden free space, slow writes to temporary
 * files (a slow save), and a failing rename of peak caches. */
class FaultOps final : public FileOps {
public:
    FileOps &b = posix_file_ops();
    std::atomic<int64_t> enospc_after{-1};  /* bytes written to .wav files (write) and to .flac / .opus files past their end
                                               (pwrite: the encoders' sink), then -ENOSPC */
    std::atomic<int64_t> free_override{-1}; /* bytes reported free for every path, when >= 0 */
    std::atomic<unsigned> tmp_delay_us{0};  /* each write to a ".tmp-" file sleeps this long */
    std::atomic<bool> fail_peak_rename{false};

    int open(const std::string &p, int f, int m) override {
        const int fd = b.open(p, f, m);
        if (fd >= 0) {
            std::lock_guard<std::mutex> g(mu_);
            if (ends_with(p, ".wav")) wav_fds_.insert(fd);
            if (ends_with(p, ".flac") || ends_with(p, ".opus")) enc_fds_.insert(fd);
            if (p.find(".tmp-") != std::string::npos) tmp_fds_.insert(fd);
        }
        return fd;
    }
    int close(int fd) override {
        {
            std::lock_guard<std::mutex> g(mu_);
            wav_fds_.erase(fd);
            enc_fds_.erase(fd);
            tmp_fds_.erase(fd);
        }
        return b.close(fd);
    }
    ssize_t write(int fd, const void *x, size_t n) override {
        if (const int e = gate(fd, n)) return e;
        return b.write(fd, x, n);
    }
    ssize_t pwrite(int fd, const void *x, size_t n, uint64_t o) override {
        if (is_tmp(fd) && tmp_delay_us.load()) std::this_thread::sleep_for(std::chrono::microseconds(tmp_delay_us.load()));
        bool enc;
        {
            std::lock_guard<std::mutex> g(mu_);
            enc = enc_fds_.count(fd) != 0;
        }
        const int64_t lim = enospc_after.load();
        if (enc && lim >= 0) { /* only what grows the file needs space */
            StatInfo st;
            b.fstat(fd, st);
            const uint64_t grow = o + n > st.size ? o + n - std::max<uint64_t>(st.size, o) : 0;
            if (grow > 0) {
                const int64_t total = written_.fetch_add(static_cast<int64_t>(grow)) + static_cast<int64_t>(grow);
                if (total > lim) return -ENOSPC;
            }
        }
        return b.pwrite(fd, x, n, o);
    }
    ssize_t pread(int fd, void *x, size_t n, uint64_t o) override { return b.pread(fd, x, n, o); }
    int fdatasync(int fd) override { return b.fdatasync(fd); }
    int fsync(int fd) override { return b.fsync(fd); }
    int ftruncate(int fd, uint64_t l) override { return b.ftruncate(fd, l); }
    int fstat(int fd, StatInfo &s) override { return b.fstat(fd, s); }
    int stat(const std::string &p, StatInfo &s) override { return b.stat(p, s); }
    int rename(const std::string &x, const std::string &y) override {
        if (fail_peak_rename.load() && ends_with(y, ".peaks")) return -EACCES;
        return b.rename(x, y);
    }
    int unlink(const std::string &p) override { return b.unlink(p); }
    int mkdir(const std::string &p) override { return b.mkdir(p); }
    int fsync_dir(const std::string &p) override { return b.fsync_dir(p); }
    int list_dir(const std::string &p, std::vector<std::string> &n) override { return b.list_dir(p, n); }
    int free_space(const std::string &p, uint64_t &a) override {
        const int64_t o = free_override.load();
        if (o >= 0) {
            a = static_cast<uint64_t>(o);
            return 0;
        }
        return b.free_space(p, a);
    }

private:
    static bool ends_with(const std::string &s, const char *suf) {
        const size_t k = std::strlen(suf);
        return s.size() >= k && s.compare(s.size() - k, k, suf) == 0;
    }
    bool is_tmp(int fd) {
        std::lock_guard<std::mutex> g(mu_);
        return tmp_fds_.count(fd) != 0;
    }
    int gate(int fd, size_t n) {
        bool wav, tmp;
        {
            std::lock_guard<std::mutex> g(mu_);
            wav = wav_fds_.count(fd) != 0;
            tmp = tmp_fds_.count(fd) != 0;
        }
        if (tmp && tmp_delay_us.load()) std::this_thread::sleep_for(std::chrono::microseconds(tmp_delay_us.load()));
        const int64_t lim = enospc_after.load();
        if (wav && lim >= 0) {
            const int64_t total = written_.fetch_add(static_cast<int64_t>(n)) + static_cast<int64_t>(n);
            if (total > lim) return -ENOSPC;
        }
        return 0;
    }
    std::mutex mu_;
    std::set<int> wav_fds_, enc_fds_, tmp_fds_;
    std::atomic<int64_t> written_{0};
};

/* Forwards to a FakeSink the test owns (the controller owns the sink it is given). */
class ForwardSink final : public PlaybackSink {
public:
    explicit ForwardSink(tp::FakeSink &s) : s_(s) {}
    int open(const AudioFormat &src, SinkFormat &granted) override { return s_.open(src, granted); }
    long write(const void *f, size_t n) override { return s_.write(f, n); }
    int recover(int e) override { return s_.recover(e); }
    long delay() override { return s_.delay(); }
    void drop() override { s_.drop(); }
    void drain() override { s_.drain(); }
    void close() override { s_.close(); }

private:
    tp::FakeSink &s_;
};

/* One controller on fakes. `home` is the temporary folder (settings and Recordings live in it). */
struct Rig {
    std::unique_ptr<tp::TempDir> own;
    std::string home;
    ToneAlsa alsa;
    TestClock clock;
    SetClockState clock_state;
    FaultOps ops;
    tp::FakeSink sink;
    ControllerConfig cfg;
    std::unique_ptr<Controller> c;

    explicit Rig(std::vector<FakeCard> cards, const std::string &existing_home = "") {
        if (existing_home.empty()) {
            own.reset(new tp::TempDir());
            home = own->path;
        } else {
            home = existing_home;
        }
        alsa.cards_ = std::move(cards);
        alsa.pace = true; /* the fake delivers in real time: a take of N seconds takes N seconds */
        cfg.api = &alsa;
        cfg.ops = &ops;
        cfg.clock = &clock;
        cfg.clock_state = &clock_state;
        cfg.home = home;
        cfg.replug_retry_ms = 150;
        cfg.service_period_ms = 10;
        cfg.make_sink = [this](const std::string &) { return std::unique_ptr<PlaybackSink>(new ForwardSink(sink)); };
    }
    ~Rig() { c.reset(); }

    std::string rec() const { return home + "/Recordings"; }
    void open() {
        c.reset(new Controller(cfg));
        c->start();
    }
    void close() { c.reset(); }
    Controller &ctl() { return *c; }
    Snapshot snap() { return c->snapshot(); }
    bool wait(const std::function<bool(const Snapshot &)> &pred, int ms = 5000) {
        return wait_until([&] { return pred(c->snapshot()); }, ms);
    }
    bool wait_state(int s, int ms = 5000) {
        return wait([s](const Snapshot &x) { return x.deck.state == s; }, ms);
    }
    bool wait_meter(int ms = 3000) {
        return wait([](const Snapshot &x) { return x.meter.channels > 0 && x.meter.ch[0].peak_db > -12.f; }, ms);
    }
    std::vector<std::string> files() {
        std::vector<std::string> n;
        posix_file_ops().list_dir(rec(), n);
        std::sort(n.begin(), n.end());
        return n;
    }
    std::vector<std::string> wavs() {
        std::vector<std::string> out;
        for (const std::string &n : files())
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) out.push_back(n);
        return out;
    }
};

inline std::vector<FakeCard> one_card() { return {fake_pcm2902(0)}; }

/* The audio of a WAV file as samples (16-bit mono or interleaved), by the header's data offset and the real length. */
struct WavData {
    bool ok = false;
    AudioFormat fmt;
    std::vector<int32_t> s;
    uint64_t frames() const { return fmt.channels ? s.size() / fmt.channels : 0; }
    bool consistent = false;
};
inline WavData read_wav(const std::string &path) {
    WavData w;
    HeaderInfo h = parse_header(posix_file_ops(), path);
    if (!h.ok || h.fmt.bits != 16) return w;
    w.fmt = h.fmt;
    w.consistent = h.consistent();
    std::vector<unsigned char> raw;
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return w;
        std::fseek(f, static_cast<long>(h.data_offset), SEEK_SET);
        raw.resize(h.actual_data);
        const size_t got = std::fread(raw.data(), 1, raw.size(), f);
        raw.resize(got);
        std::fclose(f);
    }
    uint64_t avail = raw.size();
    if (!h.suspicious() && h.declared_data < avail) avail = h.declared_data;
    for (uint64_t i = 0; i + 2 <= avail; i += 2)
        w.s.push_back(static_cast<int16_t>(raw[i] | (raw[i + 1] << 8)));
    w.ok = true;
    return w;
}

inline std::string slurp(const std::string &p) {
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

inline bool has_notice(const Snapshot &s, int kind) {
    for (const Notice &n : s.notices)
        if (n.kind == kind) return true;
    return false;
}

inline AudioFormat mono48() { return tp::afmt(48000, 1, 16); }

/* A synthetic take (the ramp of tests/playback) with Cassette's header and a valid take name. */
inline void make_take(const std::string &dir, const std::string &name, double seconds) {
    posix_file_ops().mkdir(dir);
    tp::write_wav(dir + "/" + name, mono48(), tp::ramp_samples(static_cast<uint64_t>(seconds * 48000), 1));
}

}  // namespace ta
