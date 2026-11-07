/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the demonstration GPS port (see gps_demo.hpp).
 */
#include "gps_demo.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace atlas {

DemoGpsPorts::DemoGpsPorts(const std::string &feed_path)
{
    if (feed_path == "@other") {
        other_ = true;
        return;
    }
    if (feed_path.empty()) return;
    std::ifstream in(feed_path);
    if (!in) return;
    std::int64_t due = 0;
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 6, "#wait ") == 0) {
            due += std::atol(line.c_str() + 6);
            continue;
        }
        if (line[0] == '#') continue;
        feed_.emplace_back(due, line + "\r\n");
        any = true;
    }
    if (!any) feed_.clear();
    else feed_.emplace_back(-1, std::string()); /* marker: the feed exists (see list_ports) */
}

std::vector<PortInfo> DemoGpsPorts::list_ports()
{
    std::vector<PortInfo> out;
    if (other_) {
        PortInfo o;
        o.path = "/dev/ttyUSB0";
        o.tty = "ttyUSB0";
        o.vid = 0x1a86;
        o.pid = 0x7523;
        o.product = "USB Serial";
        out.push_back(o);
        return out;
    }
    if (feed_.empty()) return out;
    PortInfo p;
    p.path = "/dev/ttyACM0";
    p.tty = "ttyACM0";
    p.vid = 0x1546;
    p.pid = 0x01a8;
    p.product = "u-blox GNSS receiver";
    out.push_back(p);
    return out;
}

int DemoGpsPorts::open(const std::string &path, PortErr &err)
{
    err = PortErr::None;
    if (other_ && path == "/dev/ttyUSB0") {
        opened_ = true;
        pending_.clear();
        pending_off_ = 0;
        t0_ = std::chrono::steady_clock::now();
        next_ = 0;
        return 42;
    }
    if (feed_.empty() || path != "/dev/ttyACM0") {
        err = PortErr::Gone;
        return -1;
    }
    opened_ = true;
    t0_ = std::chrono::steady_clock::now();
    next_ = 0;
    pending_.clear();
    pending_off_ = 0;
    return 41;
}

long DemoGpsPorts::read(int, std::uint8_t *buf, std::size_t n)
{
    if (!opened_) return -1;
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count();
    while (pending_off_ >= pending_.size() && next_ < feed_.size() && feed_[next_].first >= 0 && feed_[next_].first <= now) {
        pending_ = feed_[next_].second;
        pending_off_ = 0;
        ++next_;
    }
    if (pending_off_ >= pending_.size()) return 0;
    const std::size_t k = std::min(n, pending_.size() - pending_off_);
    for (std::size_t i = 0; i < k; ++i) buf[i] = static_cast<std::uint8_t>(pending_[pending_off_ + i]);
    pending_off_ += k;
    return static_cast<long>(k);
}

void DemoGpsPorts::close(int) { opened_ = false; }

} // namespace atlas
