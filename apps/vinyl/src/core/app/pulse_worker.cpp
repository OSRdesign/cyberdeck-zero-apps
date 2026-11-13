/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#include "pulse_worker.hpp"

#include <cstdio> /* VINYL: the volume read-back line */

namespace vinyl {

PulseWorker::PulseWorker(PulseApi &api) : api_(api) { th_ = std::thread([this] { run(); }); }

PulseWorker::~PulseWorker() { stop(); }

void PulseWorker::stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_ && !th_.joinable()) return;
        quit_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void PulseWorker::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

bool PulseWorker::request_scan(const ScanRequest &rq) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_ || scan_busy_) return false;
        scan_busy_ = true;
        jobs_.push_back([this, rq] {
            ScanResult r;
            if (rq.disconnect_first) api_.disconnect();
            if (rq.connect && !api_.connected()) r.connect_result = api_.connect(rq.app_name, rq.connect_timeout_ms);
            r.connected = api_.connected();
            if (r.connected) {
                if (rq.callback) { /* only passed while none is installed */
                    api_.set_change_callback(rq.callback);
                    r.callback_set = true;
                }
                r.listed = api_.list_sources(r.sources) >= 0 && api_.list_sinks(r.sinks) >= 0 && api_.list_cards(r.cards) >= 0;
                r.have_info = api_.server_info(r.info) == 0; /* VINYL: the default sink ("Automatic") */
            }
            std::lock_guard<std::mutex> g2(mu_);
            r.seq = ++seq_;
            scan_ = std::move(r);
            scan_ready_ = true;
            scan_busy_ = false;
        });
    }
    cv_.notify_all();
    return true;
}

bool PulseWorker::request_sink_volume(const std::string &sink, unsigned channels, uint32_t volume, int mute,
                                      const std::string &log) { /* VINYL */
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_) return false;
        level_sink_ = sink;
        level_channels_ = channels;
        level_volume_ = volume;
        level_mute_ = mute;
        level_log_ = log;
        level_have_ = true;
        if (level_busy_) return true; /* the running loop takes the newest */
        level_busy_ = true;
        jobs_.push_back([this] {
            for (;;) {
                std::string s, log;
                unsigned ch = 2;
                uint32_t vol = 0;
                int mute = -1;
                {
                    std::lock_guard<std::mutex> g2(mu_);
                    if (!level_have_ || quit_) {
                        level_busy_ = false;
                        return;
                    }
                    s = level_sink_;
                    ch = level_channels_;
                    vol = level_volume_;
                    mute = level_mute_;
                    log = level_log_;
                    level_have_ = false;
                }
                const int r = api_.set_sink_volume_raw(s, ch, vol, mute);
                uint32_t back = 0;
                bool back_muted = false;
                const int rr = r == 0 ? api_.read_sink_volume(s, back, back_muted) : r;
                if (!log.empty()) {
                    if (rr == 0)
                        std::fprintf(stderr, "[vinyl] volume: %s raw=%u read back raw=%u%s\n", log.c_str(), vol, back,
                                     back_muted ? " (muted)" : "");
                    else
                        std::fprintf(stderr, "[vinyl] volume: %s raw=%u read back failed (%d)\n", log.c_str(), vol, rr);
                }
                std::lock_guard<std::mutex> g2(mu_);
                ++levels_done_;
            }
        });
    }
    cv_.notify_all();
    return true;
}

int PulseWorker::sink_levels_done() const {
    std::lock_guard<std::mutex> g(mu_);
    return levels_done_;
}

bool PulseWorker::request_card(const std::string &name) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_ || card_busy_) return false;
        card_busy_ = true;
        jobs_.push_back([this, name] {
            CardResult r;
            r.name = name;
            r.found = api_.card_by_name(name, r.card) == 0;
            std::lock_guard<std::mutex> g2(mu_);
            r.seq = ++seq_;
            card_ = std::move(r);
            card_ready_ = true;
            card_busy_ = false;
        });
    }
    cv_.notify_all();
    return true;
}

bool PulseWorker::request_identity(std::shared_ptr<PulseCapture> stream) {
    if (!stream) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (quit_ || ident_busy_) return false;
        ident_busy_ = true;
        jobs_.push_back([this, stream] {
            IdentityResult r;
            r.ok = stream->source_identity_ok(&r.actual);
            std::lock_guard<std::mutex> g2(mu_);
            r.seq = ++seq_;
            ident_ = r;
            ident_ready_ = true;
            ident_busy_ = false;
        });
    }
    cv_.notify_all();
    return true;
}

bool PulseWorker::scan_pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return scan_busy_;
}
bool PulseWorker::card_pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return card_busy_;
}
bool PulseWorker::identity_pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return ident_busy_;
}

bool PulseWorker::take_scan(ScanResult &out) {
    std::lock_guard<std::mutex> g(mu_);
    if (!scan_ready_) return false;
    out = std::move(scan_);
    scan_ready_ = false;
    return true;
}
bool PulseWorker::take_card(CardResult &out) {
    std::lock_guard<std::mutex> g(mu_);
    if (!card_ready_) return false;
    out = std::move(card_);
    card_ready_ = false;
    return true;
}
bool PulseWorker::take_identity(IdentityResult &out) {
    std::lock_guard<std::mutex> g(mu_);
    if (!ident_ready_) return false;
    out = ident_;
    ident_ready_ = false;
    return true;
}

}  // namespace vinyl
