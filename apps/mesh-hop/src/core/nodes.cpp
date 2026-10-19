/*
 * SPDX-License-Identifier: MIT
 */

#include "nodes.hpp"

#include "tcp_transport.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <unistd.h>

namespace meshzero {

namespace {

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

/* No tab or line break may reach the file (they separate its fields). */
std::string field(const std::string &s)
{
    std::string out;
    for (char c : s) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> v;
    size_t at = 0;
    while (true) {
        const size_t t = line.find('\t', at);
        v.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos) break;
        at = t + 1;
    }
    return v;
}

bool parse_port(const std::string &s, int &port)
{
    if (s.empty() || s.size() > 5) return false;
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    port = static_cast<int>(v);
    return true;
}

} // namespace

std::string node_key(NodeKind kind, const std::string &id)
{
    switch (kind) {
    case NodeKind::Usb: return "usb:" + id;
    case NodeKind::Ble: return "ble:" + id;
    case NodeKind::Ip: return "ip:" + id;
    }
    return "";
}

bool parse_node_key(const std::string &key, NodeKind &kind, std::string &id)
{
    if (key.rfind("usb:", 0) == 0) {
        kind = NodeKind::Usb;
        id = key.substr(4);
    } else if (key.rfind("ble:", 0) == 0) {
        kind = NodeKind::Ble;
        id = key.substr(4);
    } else if (key.rfind("ip:", 0) == 0) {
        kind = NodeKind::Ip;
        id = key.substr(3);
    } else {
        return false;
    }
    return !id.empty();
}

std::string IpNode::address() const
{
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string IpNode::key() const
{
    return node_key(NodeKind::Ip, address());
}

std::string parse_ip_input(const std::string &text, int default_port, std::string &host, int &port)
{
    const std::string t = trim(text);
    if (t.empty()) return "Type the node's IP address or name";
    std::string h = t, p;
    if (t[0] == '[') {                                          // [IPv6] or [IPv6]:port
        const size_t close = t.find(']');
        if (close == std::string::npos) return "Not an IP address or host name: " + t;
        h = t.substr(1, close - 1);
        const std::string rest = t.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return "Not an IP address or host name: " + t;
            p = rest.substr(1);
            if (p.empty()) return "The port is a number from 1 to 65535";
        }
    } else if (std::count(t.begin(), t.end(), ':') == 1) {       // host:port
        const size_t c = t.find(':');
        h = t.substr(0, c);
        p = t.substr(c + 1);
        if (p.empty()) return "The port is a number from 1 to 65535";
    }                                                           // more colons: a bare IPv6 address
    int pv = default_port;
    if (!p.empty() && !parse_port(p, pv)) return "The port is a number from 1 to 65535";
    if (h.find(' ') != std::string::npos || !valid_host(h)) return "Not an IP address or host name: " + h;
    host = h;
    port = pv;
    return "";
}

std::string validate_node_name(const std::string &name)
{
    if (name.empty()) return "";
    if (utf8_length(name) > 32) return "At most 32 characters";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return "No control characters";
    if (sanitize_utf8(name) != name) return "Not valid text";
    return "";
}

int signal_bars(int rssi_dbm)
{
    static constexpr int kThresholds[4] = {1, 30, 60, 80};    // cp0_statusbar.c kBarThresholds (percent)
    if (rssi_dbm >= 0) return 0;                               // "no reading" (an RSSI is negative)
    const int pct = std::clamp(2 * (rssi_dbm + 100), 0, 100);  // cp0_ui_status_signal_percent for a negative value
    int bars = 0;
    for (int t : kThresholds)
        if (pct >= t) ++bars;
    return bars;
}

NodeBook::NodeBook(std::string dir) : path_(std::move(dir) + "/nodes.txt") {}

bool NodeBook::load()
{
    ip_.clear();
    names_.clear();
    last_.clear();
    scan_ = Scan::Ask;
    std::ifstream in(path_);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = split_tabs(line);
        if (f[0] == "ip" && f.size() >= 3) {
            IpNode n;
            n.host = f[1];
            if (!parse_port(f[2], n.port) || !valid_host(n.host)) continue;
            n.name = f.size() >= 4 ? f[3] : "";
            if (validate_node_name(n.name).empty() && !find_ip(n.key())) ip_.push_back(n);
        } else if (f[0] == "last" && f.size() >= 2) {
            NodeKind k;
            std::string id;
            if (parse_node_key(f[1], k, id)) last_ = f[1];
        } else if (f[0] == "ble_scan" && f.size() >= 2) {
            scan_ = f[1] == "yes" ? Scan::Yes : f[1] == "no" ? Scan::No : Scan::Ask;
        } else if (f[0] == "name" && f.size() >= 3) {
            NodeKind k;
            std::string id;
            if (parse_node_key(f[1], k, id) && !f[2].empty()) set_name(f[1], f[2]);
        }
    }
    return true;
}

bool NodeBook::save() const
{
    const std::string tmp = path_ + ".tmp." + std::to_string(::getpid());     // two apps at once never mix their writes
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "# Mesh Hop: the ways to reach a node (written by the app)\n";
        if (scan_ != Scan::Ask) out << "ble_scan\t" << (scan_ == Scan::Yes ? "yes" : "no") << "\n";
        if (!last_.empty()) out << "last\t" << field(last_) << "\n";
        for (const IpNode &n : ip_) out << "ip\t" << field(n.host) << "\t" << n.port << "\t" << field(n.name) << "\n";
        for (const auto &kv : names_) out << "name\t" << field(kv.first) << "\t" << field(kv.second) << "\n";
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), path_.c_str()) == 0;
}

std::string NodeBook::add_ip(const std::string &host, int port, const std::string &name)
{
    if (!valid_host(host)) return "Not an IP address or host name: " + host;
    if (port < 1 || port > 65535) return "The port is a number from 1 to 65535";
    const std::string bad = validate_node_name(name);
    if (!bad.empty()) return bad;
    IpNode n;
    n.host = host;
    n.port = port;
    n.name = name;
    if (find_ip(n.key())) return "This node is in the list already";
    if (ip_.size() >= 32) return "At most 32 nodes by IP address";
    ip_.push_back(n);
    return "";
}

bool NodeBook::remove_ip(const std::string &key)
{
    const auto it = std::find_if(ip_.begin(), ip_.end(), [&](const IpNode &n) { return n.key() == key; });
    if (it == ip_.end()) return false;
    ip_.erase(it);
    forget_name(key);
    if (last_ == key) last_.clear();
    return true;
}

const IpNode *NodeBook::find_ip(const std::string &key) const
{
    for (const IpNode &n : ip_)
        if (n.key() == key) return &n;
    return nullptr;
}

std::string NodeBook::name_of(const std::string &key) const
{
    for (const auto &kv : names_)
        if (kv.first == key) return kv.second;
    return "";
}

void NodeBook::set_name(const std::string &key, const std::string &name)
{
    for (auto &kv : names_)
        if (kv.first == key) {
            kv.second = name;
            return;
        }
    if (names_.size() >= 64) names_.erase(names_.begin());
    names_.emplace_back(key, name);
}

void NodeBook::forget_name(const std::string &key)
{
    names_.erase(std::remove_if(names_.begin(), names_.end(), [&](const auto &kv) { return kv.first == key; }), names_.end());
}

} // namespace meshzero
