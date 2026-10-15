/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the TCP port check (see portcheck.hpp).
 */
#include "portcheck.hpp"

#include "sock.hpp"
#include "text.hpp"

#include <algorithm>
#include <cerrno>

namespace nt {

const char *port_state_name(PortState s)
{
    switch (s) {
    case PortState::Open: return "open";
    case PortState::Closed: return "closed";
    case PortState::Filtered: return "filtered";
    case PortState::Unreachable: return "unreachable";
    case PortState::Error: return "error";
    default: return "...";
    }
}

PortState classify_connect(int err)
{
    switch (err) {
    case 0: return PortState::Open;
    case ECONNREFUSED:
    case ECONNRESET: return PortState::Closed;
    case ETIMEDOUT: return PortState::Filtered;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EHOSTDOWN:
    case ENETDOWN:
    case EACCES:
    case EPERM: return PortState::Unreachable;
    default: return PortState::Error;
    }
}

const std::vector<PortPreset> &port_presets()
{
    static const std::vector<PortPreset> presets = {
        {"Web", {80, 443}},
        {"Mail", {25, 465, 587, 993}},
        {"SSH", {22}},
        {"DNS", {53}},
        {"Remote desktop", {3389}},
        {"VNC", {5900}},
        {"File sharing", {445}},
        {"MQTT", {1883}},
        {"Printer", {631, 9100}},
        {"HTTP alt", {8080, 8443}},
    };
    return presets;
}

const char *port_service(std::uint16_t port)
{
    switch (port) {
    case 21: return "ftp";
    case 22: return "ssh";
    case 23: return "telnet";
    case 25: return "smtp";
    case 53: return "dns";
    case 80: return "http";
    case 110: return "pop3";
    case 143: return "imap";
    case 443: return "https";
    case 445: return "smb";
    case 465: return "smtps";
    case 587: return "submission";
    case 631: return "ipp";
    case 993: return "imaps";
    case 995: return "pop3s";
    case 1883: return "mqtt";
    case 3389: return "rdp";
    case 5900: return "vnc";
    case 8080: return "http-alt";
    case 8443: return "https-alt";
    case 9100: return "printer";
    default: return "";
    }
}

PortResult check_port(const Address &addr, std::uint16_t port, int timeout_ms, const CancelToken &cancel)
{
    PortResult r;
    r.port = port;
    const std::int64_t start = now_us();
    int err = 0;
    Fd fd = tcp_connect(addr, port, now_ms() + std::max(100, timeout_ms), cancel, &err);
    r.connect_ms = static_cast<int>((now_us() - start + 500) / 1000);
    r.err = err;
    if (err == ECANCELED) {
        r.state = PortState::Pending;
        r.detail = "cancelled";
        return r;
    }
    r.state = classify_connect(err);
    r.detail = err ? errno_words(err) : "connected";
    if (r.state != PortState::Open && r.state != PortState::Closed) r.connect_ms = 0;
    return r;
}

bool PortSession::start(const PortRequest &req)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    PortSnapshot first;
    first.host = trim(req.host);
    std::string why;
    std::vector<std::uint16_t> ports;
    for (const std::uint16_t p : req.ports)
        if (p && std::find(ports.begin(), ports.end(), p) == ports.end() && ports.size() < 16) ports.push_back(p);
    if (!valid_host(first.host, &why) || ports.empty()) {
        first.state = PortSnapshot::State::Failed;
        first.error = ports.empty() ? "Choose a port (1 to 65535)" : why;
        channel_.begin(gen, first);
        return false;
    }
    for (const std::uint16_t p : ports) {
        PortResult r;
        r.port = p;
        first.results.push_back(r);
    }
    first.state = PortSnapshot::State::Resolving;
    channel_.begin(gen, first);
    const std::uint64_t started = worker_.start([this, gen, first, req](const CancelToken &cancel) {
        PortSnapshot snap = first;
        const Resolved res = resolve_host(snap.host, req.resolve, cancel);
        if (cancel.cancelled()) {
            snap.state = PortSnapshot::State::Cancelled;
            channel_.publish(gen, snap);
            return;
        }
        if (!res.ok) {
            snap.state = PortSnapshot::State::Failed;
            snap.error = "Cannot find " + snap.host + ": " + res.error;
            channel_.publish(gen, snap);
            return;
        }
        snap.address = res.address.text();
        snap.resolved_by = res.literal ? "" : res.from_hosts ? "hosts" : res.server;
        snap.state = PortSnapshot::State::Running;
        channel_.publish(gen, snap);
        for (PortResult &r : snap.results) {
            if (cancel.cancelled()) break;
            r = check_port(res.address, r.port, req.timeout_ms, cancel);
            if (cancel.cancelled()) break;
            channel_.publish(gen, snap);
        }
        snap.state = cancel.cancelled() ? PortSnapshot::State::Cancelled : PortSnapshot::State::Done;
        channel_.publish(gen, snap);
    });
    if (!started) {
        first.state = PortSnapshot::State::Failed;
        first.error = "Could not start the check (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void PortSession::stop()
{
    worker_.cancel();
}

} // namespace nt
