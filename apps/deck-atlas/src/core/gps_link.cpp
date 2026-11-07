/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS reader's state machine (see gps_link.hpp).
 */
#include "gps_link.hpp"

#include <algorithm>
#include <cctype>

namespace atlas {

namespace {

constexpr int kReadsPerTick = 4;       /* at most 4 x 512 bytes per 100 ms tick: a receiver sends about 500 bytes a second */
constexpr std::size_t kReadSize = 512;
constexpr int kLockSentences = 2;      /* valid-checksum sentences needed to call a port a receiver */

bool contains_ci(const std::string &hay, const char *needle)
{
    std::string h = hay;
    for (char &c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(needle) != std::string::npos;
}

/* Lower is tried first: a u-blox (USB 1546), a port named like a GPS, then native USB (ACM), then bridges (USB). */
int port_score(const PortInfo &p)
{
    if (p.vid == 0x1546) return 0;
    if (contains_ci(p.by_id, "gps") || contains_ci(p.by_id, "gnss") || contains_ci(p.by_id, "u-blox") ||
        contains_ci(p.product, "gps") || contains_ci(p.product, "gnss"))
        return 1;
    if (p.tty.compare(0, 6, "ttyACM") == 0) return 2;
    if (p.tty.compare(0, 6, "ttyUSB") == 0) return 3;
    return 4;
}

} // namespace

GpsLink::~GpsLink() { close_port(); }

std::int64_t GpsLink::backoff_ms(int failures)
{
    static const std::int64_t kSteps[] = {2000, 4000, 8000, 15000, 30000};
    const int i = std::min(std::max(failures, 0), 4);
    return kSteps[i];
}

std::vector<int> GpsLink::baud_order(int last)
{
    std::vector<int> v;
    if (last > 0) v.push_back(last);
    for (int b : {9600, 4800, 38400, 115200})
        if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
    return v;
}

void GpsLink::configure(const GpsConfig &cfg)
{
    if (cfg == cfg_) return;
    close_port();
    cfg_ = cfg;
    tracker_.reset();
    fails_.clear();
    last_listing_.clear();
    next_scan_ms_ = 0;
    state_ = cfg_.on ? GpsLinkState::Searching : GpsLinkState::Off;
    say(cfg_.on ? "gps: on (port " + (cfg_.port.empty() ? std::string("auto") : cfg_.port) + ", speed " +
                      (cfg_.baud ? std::to_string(cfg_.baud) : std::string("auto")) + ")"
                : std::string("gps: off"));
}

void GpsLink::close_port()
{
    if (handle_ >= 0) sys_.close(handle_);
    handle_ = -1;
    port_ = PortInfo();
    baud_ = 0;
    bauds_.clear();
    baud_idx_ = 0;
    valid_count_ = 0;
    pending_.clear();
    splitter_.reset();
}

void GpsLink::fail_port(const std::string &path, PortErr err, std::int64_t now)
{
    PortFail &f = fails_[path];
    f.err = err;
    f.retry_at = now + backoff_ms(f.count);
    ++f.count;
}

void GpsLink::lose(const char *why, std::int64_t now)
{
    const std::string path = port_.path;
    const bool gone = std::string(why) != "silent";
    say("gps: lost " + path + " (" + why + ")");
    close_port();
    fail_port(path, PortErr::None, now);
    state_ = gone ? GpsLinkState::NoDevice : GpsLinkState::Searching;
    next_scan_ms_ = now; /* look at the list at once */
}

bool GpsLink::begin(const PortInfo &p, std::int64_t now, PortErr &err)
{
    const int h = sys_.open(p.path, err);
    if (h < 0) return false;
    const std::vector<int> order = cfg_.baud > 0 ? std::vector<int>{cfg_.baud} : baud_order(last_good_baud_);
    if (!sys_.configure(h, order[0])) {
        sys_.close(h);
        err = PortErr::Other;
        return false;
    }
    sys_.discard_input(h);
    handle_ = h;
    port_ = p;
    bauds_ = order;
    baud_idx_ = 0;
    baud_ = order[0];
    splitter_.reset();
    pending_.clear();
    valid_count_ = 0;
    sniff_start_ = now;
    last_valid_ms_ = now;
    state_ = GpsLinkState::Searching;
    say("gps: opened " + p.path + " at " + std::to_string(baud_));
    return true;
}

void GpsLink::scan(std::int64_t now)
{
    const std::vector<PortInfo> ports = sys_.list_ports();
    std::vector<std::string> listing;
    for (const PortInfo &p : ports) listing.push_back(p.path);
    if (listing != last_listing_) { /* hot-plug: what left is forgotten (a device that comes back starts over) */
        for (auto it = fails_.begin(); it != fails_.end();) {
            if (std::find(listing.begin(), listing.end(), it->first) == listing.end()) it = fails_.erase(it);
            else ++it;
        }
        last_listing_ = listing;
    }
    next_scan_ms_ = now + timing_.scan_ms;

    std::vector<PortInfo> cand;
    if (!cfg_.port.empty()) {
        for (const PortInfo &p : ports)
            if (p.path == cfg_.port) cand.push_back(p);
    } else {
        cand = ports;
        std::stable_sort(cand.begin(), cand.end(),
                         [](const PortInfo &a, const PortInfo &b) { return port_score(a) < port_score(b); });
    }
    if (cand.empty()) {
        state_ = GpsLinkState::NoDevice;
        return;
    }
    int denied = 0, other = 0;
    for (const PortInfo &c : cand) {
        const auto f = fails_.find(c.path);
        if (f != fails_.end() && f->second.retry_at > now) {
            (f->second.err == PortErr::Denied ? denied : other)++;
            continue;
        }
        PortErr err = PortErr::None;
        if (begin(c, now, err)) return;
        if (err == PortErr::Gone) continue; /* gone between the listing and the open */
        fail_port(c.path, err, now);
        (err == PortErr::Denied ? denied : other)++;
    }
    state_ = (denied > 0 && other == 0) ? GpsLinkState::NoAccess : GpsLinkState::Searching;
    if (denied == 0 && other == 0) state_ = GpsLinkState::NoDevice;
}

void GpsLink::read_some(std::int64_t now)
{
    std::uint8_t buf[kReadSize];
    for (int i = 0; i < kReadsPerTick; ++i) {
        const long n = sys_.read(handle_, buf, sizeof buf);
        if (n < 0) {
            lose("read error", now);
            return;
        }
        if (n == 0) return;
        std::vector<std::string> lines;
        splitter_.feed(buf, static_cast<std::size_t>(n), lines);
        for (const std::string &line : lines) {
            const NmeaMsg m = parse_nmea(line);
            if (m.kind == MsgKind::None) continue; /* bad checksum or not a sentence */
            ++valid_count_;
            last_valid_ms_ = now;
            if (state_ == GpsLinkState::Locked) tracker_.on_msg(m, now);
            else pending_.push_back(m);
        }
    }
}

void GpsLink::next_baud_or_fail(std::int64_t now)
{
    if (++baud_idx_ < bauds_.size()) {
        baud_ = bauds_[baud_idx_];
        if (!sys_.configure(handle_, baud_)) {
            lose("cannot set the speed", now);
            return;
        }
        sys_.discard_input(handle_);
        splitter_.reset();
        pending_.clear();
        valid_count_ = 0;
        sniff_start_ = now;
        return;
    }
    const std::string path = port_.path;
    say("gps: " + path + " does not talk NMEA");
    close_port();
    fail_port(path, PortErr::None, now);
    state_ = GpsLinkState::Searching;
    next_scan_ms_ = now; /* the next candidate, if any, at once */
}

void GpsLink::tick(std::int64_t now)
{
    if (!cfg_.on) return;
    if (handle_ < 0) {
        if (now >= next_scan_ms_) scan(now);
        return;
    }
    read_some(now);
    if (handle_ < 0) return;
    if (state_ == GpsLinkState::Searching) {
        if (valid_count_ >= kLockSentences) {
            tracker_.reset();
            for (const NmeaMsg &m : pending_) tracker_.on_msg(m, now);
            pending_.clear();
            state_ = GpsLinkState::Locked;
            last_good_baud_ = baud_;
            fails_.erase(port_.path);
            last_valid_ms_ = now;
            say("gps: " + port_.path + " talks NMEA at " + std::to_string(baud_));
        } else if (now - sniff_start_ >= timing_.sniff_ms) {
            next_baud_or_fail(now);
        }
    } else if (now - last_valid_ms_ >= timing_.silence_ms) {
        lose("silent", now);
    }
}

} // namespace atlas
