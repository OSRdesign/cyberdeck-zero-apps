/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the link to the sensor (see link.hpp).
 */
#include "link.hpp"

#include <cstring>

namespace mmw {

void DiagFrameCounts::count(const Frame &f)
{
    if (f.present_count() == 0) {
        ++empty;
        ++total_empty;
        return;
    }
    ++nonempty;
    ++total_nonempty;
    const std::uint8_t *t = f.raw + 4; /* the three 8-byte target slots */
    if (have_prev && std::memcmp(prev, t, sizeof(prev)) == 0) {
        ++same;
        ++total_same;
    }
    std::memcpy(prev, t, sizeof(prev));
    have_prev = true;
    std::memcpy(latest, f.raw, sizeof(latest));
    latest_new = true;
}

const char *link_state_name(LinkState s)
{
    switch (s) {
    case LinkState::Searching: return "Searching";
    case LinkState::Found: return "Found";
    case LinkState::Lost: return "Lost";
    case LinkState::Busy: return "Busy";
    case LinkState::Error: return "Error";
    }
    return "?";
}

Link::Link(Sys &sys, const LinkConfig &cfg) : sys_(sys), cfg_(cfg), det_(sys, cfg.detect), tracker_(cfg.tracker) {}

Link::~Link() { close(); }

void Link::close()
{
    det_.cancel();
    if (open_) {
        sys_.restore(port_.handle);
        sys_.close(port_.handle);
        open_ = false;
        port_.handle = -1;
    }
    started_ = false;
    scanning_ = false;
}

void Link::start(Ms now, const Remembered &remembered)
{
    close();
    remembered_ = remembered;
    remembered_changed_ = false;
    settled_.clear();
    listed_.clear();
    reports_.clear();
    ever_found_ = false;
    driver_ = nullptr;
    dec_.reset();
    stats_ = LinkStats();
    last_frame_ms_ = -1;
    tracker_.clear();
    tty_settings_.clear();
    first_target_frames_.clear();
    diag_ = DiagFrameCounts();
    diag_.hex_since = now;
    state_ = LinkState::Searching;
    started_ = true;
    begin_scan(now);
}

void Link::rescan(Ms now)
{
    if (!started_) return;
    if (open_) return; /* streaming: nothing to scan */
    det_.cancel();
    settled_.clear();
    begin_scan(now);
}

void Link::begin_scan(Ms now)
{
    ports_gen_ = sys_.ports_generation();
    listed_.clear();
    for (const PortMeta &m : sys_.list_ports()) listed_.insert(m.path);
    det_.start(now, remembered_, settled_);
    scanning_ = true;
    last_scan_ms_ = now;
    idle_check_ms_ = now;
}

Ms Link::lost_after_ms() const
{
    if (cfg_.lost_ms > 0) return cfg_.lost_ms;
    return driver_ ? driver_->quiet_ms : kDriverQuietDefaultMs;
}

const char *Link::model() const
{
    if (driver_) return driver_->model;
    std::size_t n = 0;
    const SensorDriver *const *all = drivers(n);
    for (std::size_t i = 0; i < n; ++i)
        if (all[i]->supported()) return all[i]->model;
    return "";
}

void Link::set_sensor_type(const std::string &id, Ms now)
{
    if (cfg_.detect.sensor == id) return;
    cfg_.detect.sensor = id;
    det_.set_sensor(id);
    if (started_ && !open_) rescan(now);
}

std::string Link::status() const { return scanning_ ? det_.status() : std::string(); }

LinkStats Link::stats(Ms now) const
{
    LinkStats s = stats_;
    s.last_frame_age_ms = last_frame_ms_ < 0 ? -1 : now - last_frame_ms_;
    return s;
}

bool Link::take_remembered_changed()
{
    const bool c = remembered_changed_;
    remembered_changed_ = false;
    return c;
}

void Link::set_remembered_ble(const std::string &address, const std::string &name)
{
    if (remembered_.ble_address == address && remembered_.ble_name == name) return;
    remembered_.ble_address = address;
    remembered_.ble_name = name;
    remembered_changed_ = true;
}

void Link::lose(Ms now, const char *reason, Ms quiet_ms)
{
    if (events_ && port_.meta.transport == Transport::Ble) {
        std::string l = "t=" + std::to_string(now) + " ble-lost reason=" + reason + " addr=" + port_.meta.address;
        if (quiet_ms > 0) l += " quiet_ms=" + std::to_string(quiet_ms) + " limit_ms=" + std::to_string(lost_after_ms());
        events_(l);
    }
    if (open_) {
        sys_.restore(port_.handle);
        sys_.close(port_.handle);
        open_ = false;
        port_.handle = -1;
    }
    stats_.fps = 0.0;
    arrivals_.clear();
    state_ = LinkState::Lost;
    tracker_.tick(now);
    settled_.clear(); /* the sensor may be on another port number now */
    begin_scan(now);
}

void Link::read_frames(Ms now)
{
    std::uint8_t buf[1024];
    std::vector<Frame> frames;
    for (int rounds = 0; rounds < 4; ++rounds) {
        const long n = sys_.read(port_.handle, buf, sizeof(buf));
        if (n < 0) {
            lose(now, "port-gone");
            return;
        }
        if (n == 0) break;
        dec_->feed(buf, static_cast<std::size_t>(n), &frames);
        if (n < static_cast<long>(sizeof(buf))) break;
    }
    for (Frame &f : frames) {
        if (f.present_count() > 0 && first_target_frames_.size() < 3) {
            static const char *hex = "0123456789abcdef";
            std::string h;
            for (int k = 0; k < 30; ++k) {
                h += hex[f.raw[k] >> 4];
                h += hex[f.raw[k] & 15];
            }
            first_target_frames_.push_back(h);
        }
        if (diag_on_) diag_.count(f);
        tracker_.update(f, now);
        last_ = f;
        arrivals_.push_back(now);
        last_frame_ms_ = now;
    }
    const DecoderStats &ds = dec_->stats();
    stats_.frames = ds.frames_ok;
    stats_.bad_frames = ds.frames_bad + ds.frames_cut;
    stats_.answers = ds.answers;
    stats_.bytes = ds.bytes;
    while (!arrivals_.empty() && now - arrivals_.front() > 1000) arrivals_.pop_front();
    const Ms span = now - found_ms_ < 1000 ? (now - found_ms_ > 0 ? now - found_ms_ : 1) : 1000;
    stats_.fps = static_cast<double>(arrivals_.size()) * 1000.0 / static_cast<double>(span);
    const Ms lost_ms = lost_after_ms();
    if (last_frame_ms_ < 0 ? now - found_ms_ > lost_ms : now - last_frame_ms_ > lost_ms)
        lose(now, last_frame_ms_ < 0 ? "no-frame-since-found" : "quiet", last_frame_ms_ < 0 ? now - found_ms_ : now - last_frame_ms_);
}

void Link::after_scan(Ms now)
{
    scanning_ = false;
    reports_ = det_.reports();
    bool busy = false, err = false;
    for (const PortReport &r : reports_) {
        if (r.verdict == Verdict::Busy) busy = true;
        if (r.verdict == Verdict::Denied || r.verdict == Verdict::Error || r.verdict == Verdict::Gone) err = true;
        /* the remembered radar's own port is never settled: a radar that went quiet comes back */
        if ((r.verdict == Verdict::NotRadar || r.verdict == Verdict::Silent) && r.id != remembered_.port_id)
            settled_.insert(r.meta.path);
    }
    if (det_.found()) {
        port_ = det_.take_radar();
        open_ = true;
        tty_settings_ = sys_.describe(port_.handle);
        driver_ = port_.driver;
        dec_ = driver_->make_decoder();
        tracker_.set_drop_ms(driver_->drop_ms);
        stats_ = LinkStats();
        arrivals_.clear();
        last_frame_ms_ = -1;
        found_ms_ = now;
        ever_found_ = true;
        state_ = LinkState::Found;
        if (events_ && port_.meta.transport == Transport::Ble)
            events_("t=" + std::to_string(now) + " ble-found addr=" + port_.meta.address + " name=" +
                    (port_.meta.tty.empty() ? "-" : port_.meta.tty) + " driver=" + driver_->id);
        if (port_.meta.transport == Transport::Ble) return; /* the Bluetooth radar is remembered when the user chooses it */
        if (port_.id != remembered_.port_id || port_.baud != remembered_.baud) {
            remembered_.port_id = port_.id;
            remembered_.baud = port_.baud;
            remembered_changed_ = true;
        }
        return;
    }
    if (busy) state_ = LinkState::Busy;
    else if (err) state_ = LinkState::Error;
    else if (ever_found_) state_ = LinkState::Lost;
    else state_ = LinkState::Searching;
}

void Link::poll(Ms now)
{
    if (!started_) return;
    if (scanning_) {
        det_.poll(now);
        if (det_.finished()) after_scan(now);
        else return;
        if (state_ == LinkState::Found) return;
    }
    if (open_) {
        read_frames(now);
        tracker_.tick(now);
        return;
    }
    tracker_.tick(now);
    /* idle: no radar. Hot-plug check every rescan_ms, and at once when the port layer says its list changed. */
    const std::uint64_t gen = sys_.ports_generation();
    if (now - idle_check_ms_ < cfg_.rescan_ms && gen == ports_gen_) return;
    ports_gen_ = gen;
    idle_check_ms_ = now;
    std::set<std::string> listed;
    for (const PortMeta &m : sys_.list_ports()) listed.insert(m.path);
    /* forget settled ports that went away (a replug is a new device) */
    for (auto it = settled_.begin(); it != settled_.end();) {
        if (!listed.count(*it)) it = settled_.erase(it);
        else ++it;
    }
    bool unsettled_new = false;
    for (const std::string &p : listed)
        if (!settled_.count(p)) unsettled_new = true;
    const bool retry_due = (state_ == LinkState::Busy || state_ == LinkState::Error || state_ == LinkState::Lost) &&
                           now - last_scan_ms_ >= cfg_.retry_ms;
    if (unsettled_new && (listed != listed_ || retry_due)) {
        listed_ = listed;
        begin_scan(now);
    } else {
        listed_ = listed;
    }
}

} // namespace mmw
