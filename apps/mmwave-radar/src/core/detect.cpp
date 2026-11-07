/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the listen-only port detector (see detect.hpp).
 */
#include "detect.hpp"

#include <algorithm>
#include <cstdlib>

namespace mmw {

std::string Remembered::serialize() const
{
    std::string out;
    if (!empty()) out = "port=" + port_id + "\nbaud=" + std::to_string(baud) + "\n";
    if (!ble_empty()) out += "ble=" + ble_address + "\nble_name=" + ble_name + "\n";
    return out;
}

Remembered Remembered::parse(const std::string &text)
{
    Remembered r;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "port=") == 0) r.port_id = line.substr(5);
        else if (line.compare(0, 5, "baud=") == 0) r.baud = std::atoi(line.c_str() + 5);
        else if (line.compare(0, 9, "ble_name=") == 0) r.ble_name = line.substr(9);
        else if (line.compare(0, 4, "ble=") == 0) r.ble_address = line.substr(4);
    }
    if (r.baud < 1200 || r.baud > 3000000) {
        r.port_id.clear();
        r.baud = 0;
    }
    if (!valid_ble_address(r.ble_address)) {
        r.ble_address.clear();
        r.ble_name.clear();
    }
    if (r.ble_name.size() > 64) r.ble_name.resize(64);
    return r;
}

Detector::~Detector()
{
    if (handle_ >= 0) {
        sys_.restore(handle_);
        sys_.close(handle_);
        handle_ = -1;
    }
    if (radar_.handle >= 0) sys_.close(radar_.handle); /* found but never taken */
}

void Detector::start(Ms now, const Remembered &remembered, const std::set<std::string> &settled)
{
    cancel();
    if (radar_.handle >= 0) sys_.close(radar_.handle);
    radar_ = RadarPort();
    found_ = false;
    remembered_ = remembered;
    reports_.clear();
    queue_.clear();
    qi_ = 0;
    start_ms_ = now;
    std::vector<PortMeta> all = sys_.list_ports();
    for (const PortMeta &m : all)
        if (!settled.count(m.path)) queue_.push_back(m);
    /* a Bluetooth radar (listed only while connected) before the other ports, the remembered port first */
    std::stable_partition(queue_.begin(), queue_.end(), [](const PortMeta &m) { return m.transport == Transport::Ble; });
    if (!remembered.port_id.empty()) {
        std::stable_partition(queue_.begin(), queue_.end(),
                              [&](const PortMeta &m) { return port_id(m) == remembered.port_id; });
    }
    state_ = State::NextPort;
}

void Detector::cancel()
{
    if (handle_ >= 0) {
        sys_.restore(handle_);
        sys_.close(handle_);
        handle_ = -1;
    }
    if (state_ == State::Listen || state_ == State::NextPort) {
        /* the port in progress (if any) and the rest are skipped */
        if (state_ == State::Listen && qi_ < queue_.size()) {
            PortReport r;
            r.meta = queue_[qi_];
            r.id = port_id(r.meta);
            r.title = port_title(r.meta);
            r.verdict = Verdict::Skipped;
            r.text = "not finished";
            reports_.push_back(r);
            ++qi_;
        }
        for (; qi_ < queue_.size(); ++qi_) {
            PortReport r;
            r.meta = queue_[qi_];
            r.id = port_id(r.meta);
            r.title = port_title(r.meta);
            r.verdict = Verdict::Skipped;
            r.text = "not tried (scan stopped)";
            reports_.push_back(r);
        }
        state_ = State::Done;
    }
}

std::string Detector::status() const
{
    if (state_ != State::Listen || qi_ >= queue_.size() || bi_ >= bauds_.size()) return std::string();
    if (nobaud_) return "Listening to " + port_title(queue_[qi_]);
    return "Looking at " + port_title(queue_[qi_]) + " at " + std::to_string(bauds_[bi_]) + " baud";
}

RadarPort Detector::take_radar()
{
    RadarPort r = radar_;
    radar_ = RadarPort();
    found_ = false;
    return r;
}

void Detector::end_port(Verdict v, const std::string &text, int baud)
{
    PortReport r;
    r.meta = queue_[qi_];
    r.id = port_id(r.meta);
    r.title = port_title(r.meta);
    r.verdict = v;
    r.text = text;
    r.baud = baud;
    reports_.push_back(r);
    if (handle_ >= 0 && v != Verdict::Radar) {
        if (v != Verdict::Gone) sys_.restore(handle_);
        sys_.close(handle_);
    }
    handle_ = -1;
    ++qi_;
    state_ = State::NextPort;
}

void Detector::finish(Ms)
{
    state_ = State::Done;
}

void Detector::begin_port(Ms now)
{
    if (qi_ >= queue_.size() || found_) {
        /* anything after a found radar is not looked at */
        for (; qi_ < queue_.size(); ++qi_) {
            PortReport r;
            r.meta = queue_[qi_];
            r.id = port_id(r.meta);
            r.title = port_title(r.meta);
            r.verdict = Verdict::Skipped;
            r.text = "not tried (radar found)";
            reports_.push_back(r);
        }
        finish(now);
        return;
    }
    if (now - start_ms_ > cfg_.total_budget_ms) {
        for (; qi_ < queue_.size(); ++qi_) {
            PortReport r;
            r.meta = queue_[qi_];
            r.id = port_id(r.meta);
            r.title = port_title(r.meta);
            r.verdict = Verdict::Skipped;
            r.text = "not tried (time budget)";
            reports_.push_back(r);
        }
        finish(now);
        return;
    }
    const PortMeta &m = queue_[qi_];
    if (sys_.held_by_other(m.path)) {
        end_port(Verdict::Busy, "busy (held by another app)", 0);
        return;
    }
    OpenErr err = OpenErr::None;
    handle_ = sys_.open(m.path, err);
    if (handle_ < 0) {
        switch (err) {
        case OpenErr::Busy: end_port(Verdict::Busy, "busy (held by another app)", 0); break;
        case OpenErr::Denied:
            end_port(Verdict::Denied, "permission denied (add yourself to the dialout group)", 0);
            break;
        case OpenErr::Gone: end_port(Verdict::Gone, "gone while opening", 0); break;
        default: end_port(Verdict::Error, "cannot be opened", 0); break;
        }
        return;
    }
    choose_drivers();
    /* the speed order: this port's remembered speed first, then the list; a port without a speed: one pass (0) */
    bauds_.clear();
    nobaud_ = m.transport == Transport::Ble;
    if (nobaud_) bauds_.push_back(0);
    else if (!remembered_.empty() && remembered_.port_id == port_id(m)) bauds_.push_back(remembered_.baud);
    auto add_baud = [&](int b) {
        if (std::find(bauds_.begin(), bauds_.end(), b) == bauds_.end()) bauds_.push_back(b);
    };
    if (nobaud_) {
        /* nothing to add */
    } else if (!cfg_.bauds.empty()) {
        for (int b : cfg_.bauds) add_baud(b);
    } else {
        for (const SensorDriver *d : active_)
            for (std::size_t k = 0; k < d->baud_count; ++k) add_baud(d->bauds[k]);
    }
    bi_ = 0;
    first_baud_ = true;
    any_bytes_ = false;
    sniff_.reset();
    state_ = State::Listen;
    begin_baud(now);
}

/* The drivers in play on this port: the one the Sensor type names, else every real driver; the stubs are looked for
 * (named in the scan) only with Auto. */
void Detector::choose_drivers()
{
    active_.clear();
    decs_.clear();
    const SensorDriver *fixed = find_driver(cfg_.sensor.c_str());
    if (fixed && !fixed->supported()) fixed = nullptr;
    std::size_t n = 0;
    const SensorDriver *const *all = drivers(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!all[i]->supported() || (fixed && all[i] != fixed)) continue;
        active_.push_back(all[i]);
        decs_.push_back(all[i]->make_decoder());
    }
    sniff_.set_stubs(fixed == nullptr);
}

void Detector::begin_baud(Ms now)
{
    if (nobaud_) { /* Bluetooth: no speed, no flush (what came since the subscription is the sensor's), one window */
        for (auto &d : decs_) d->reset();
        window_start_ = now;
        window_len_ = cfg_.first_listen_ms;
        for (const SensorDriver *d : active_) window_len_ = std::max<Ms>(window_len_, d->listen_ms);
        window_len_ += cfg_.nobaud_extra_ms;
        silent_len_ = window_len_;
        return;
    }
    while (bi_ < bauds_.size() && !sys_.configure(handle_, bauds_[bi_])) ++bi_; /* a speed the driver refuses */
    if (bi_ >= bauds_.size()) {
        end_port(Verdict::Error, "cannot set any speed", 0);
        return;
    }
    sys_.flush_input(handle_);
    for (auto &d : decs_) d->reset();
    window_start_ = now;
    window_len_ = first_baud_ ? cfg_.first_listen_ms : cfg_.next_listen_ms;
    /* a driver with a slow sensor (the LD2451 sends one frame every 1.4 s when idle) asks for a longer window at its
     * own first speed, so that three frames fit; and for a longer wait before a quiet port counts as silent */
    silent_len_ = window_len_;
    for (const SensorDriver *d : active_) {
        if (d->listen_ms > 0 && d->baud_count > 0 && d->bauds[0] == bauds_[bi_])
            window_len_ = std::max<Ms>(window_len_, d->listen_ms);
        if (d->silent_ms > 0) silent_len_ = std::max<Ms>(silent_len_, d->silent_ms);
    }
}

void Detector::next_baud(Ms now)
{
    first_baud_ = false;
    ++bi_;
    if (bi_ < bauds_.size() && now - start_ms_ > cfg_.total_budget_ms) {
        end_port(Verdict::Skipped, "not a supported radar: no answer within the time budget", 0);
        return;
    }
    if (bi_ >= bauds_.size()) {
        std::string what = sniff_.describe_unknown();
        end_port(Verdict::NotRadar, "not a supported radar: " + (any_bytes_ ? what : std::string("silent")), 0);
        return;
    }
    begin_baud(now);
}

void Detector::poll(Ms now)
{
    if (state_ == State::Done || state_ == State::Idle) return;
    if (state_ == State::NextPort) {
        begin_port(now);
        if (state_ != State::Listen) return;
    }
    /* state_ == Listen */
    std::uint8_t buf[512];
    for (int rounds = 0; rounds < 8; ++rounds) {
        const long n = sys_.read(handle_, buf, sizeof(buf));
        if (n < 0) {
            end_port(Verdict::Gone, "gone while probing", 0);
            return;
        }
        if (n == 0) break;
        any_bytes_ = true;
        for (auto &d : decs_) d->feed(buf, static_cast<std::size_t>(n), nullptr);
        sniff_.feed(buf, static_cast<std::size_t>(n));
        const SensorDriver *hit = nullptr;
        for (std::size_t k = 0; k < decs_.size() && !hit; ++k)
            if (decs_[k]->stats().good_run >= cfg_.frames_needed) hit = active_[k];
        if (hit) {
            radar_.driver = hit;
            radar_.handle = handle_;
            radar_.meta = queue_[qi_];
            radar_.id = port_id(radar_.meta);
            radar_.title = port_title(radar_.meta);
            radar_.baud = bauds_[bi_];
            found_ = true;
            end_port(Verdict::Radar, hit->found_text, bauds_[bi_]); /* the handle stays open */
            begin_port(now); /* found_: the rest is skipped, the scan is done */
            return;
        }
        if (sniff_.kind() != Sniffed::None) {
            end_port(Verdict::NotRadar, "not a supported radar: " + sniff_.describe(), bauds_[bi_]);
            return;
        }
        if (n < static_cast<long>(sizeof(buf))) break;
    }
    bool header_seen = false;
    std::uint64_t bytes_seen = 0;
    for (auto &d : decs_) {
        const DecoderStats &st = d->stats();
        if (st.frames_ok + st.frames_bad + st.frames_cut + st.answers > 0) header_seen = true;
        if (st.bytes > bytes_seen) bytes_seen = st.bytes;
    }
    const Ms elapsed = now - window_start_;
    if (first_baud_ && !any_bytes_) {
        if (elapsed >= silent_len_) end_port(Verdict::Silent, "not a supported radar: silent (no data)", 0);
        return; /* a quiet sensor may still talk: wait out silent_len_ before the next speed */
    }
    if (!header_seen && bytes_seen >= cfg_.early_garbage_bytes) {
        next_baud(now);
        return;
    }
    if (elapsed >= window_len_) next_baud(now);
}

} // namespace mmw
