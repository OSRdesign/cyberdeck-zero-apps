/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the link to the sensor (see link.hpp).
 */
#include "link.hpp"

namespace mmw {

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
    stats_ = LinkStats();
    last_frame_ms_ = -1;
    tracker_.clear();
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
    listed_.clear();
    for (const PortMeta &m : sys_.list_ports()) listed_.insert(m.path);
    det_.start(now, remembered_, settled_);
    scanning_ = true;
    last_scan_ms_ = now;
    idle_check_ms_ = now;
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

void Link::lose(Ms now)
{
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
            lose(now);
            return;
        }
        if (n == 0) break;
        dec_.feed(buf, static_cast<std::size_t>(n), &frames);
        if (n < static_cast<long>(sizeof(buf))) break;
    }
    for (const Frame &f : frames) {
        tracker_.update(f, now);
        last_ = f;
        arrivals_.push_back(now);
        last_frame_ms_ = now;
    }
    const DecoderStats &ds = dec_.stats();
    stats_.frames = ds.frames_ok;
    stats_.bad_frames = ds.frames_bad + ds.frames_cut;
    stats_.answers = ds.answers;
    stats_.bytes = ds.bytes;
    while (!arrivals_.empty() && now - arrivals_.front() > 1000) arrivals_.pop_front();
    const Ms span = now - found_ms_ < 1000 ? (now - found_ms_ > 0 ? now - found_ms_ : 1) : 1000;
    stats_.fps = static_cast<double>(arrivals_.size()) * 1000.0 / static_cast<double>(span);
    if (last_frame_ms_ < 0 ? now - found_ms_ > cfg_.lost_ms : now - last_frame_ms_ > cfg_.lost_ms) lose(now);
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
        dec_.reset();
        stats_ = LinkStats();
        arrivals_.clear();
        last_frame_ms_ = -1;
        found_ms_ = now;
        ever_found_ = true;
        state_ = LinkState::Found;
        Remembered r;
        r.port_id = port_.id;
        r.baud = port_.baud;
        if (r.port_id != remembered_.port_id || r.baud != remembered_.baud) {
            remembered_ = r;
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
    /* idle: no radar. Hot-plug check every rescan_ms. */
    if (now - idle_check_ms_ < cfg_.rescan_ms) return;
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
