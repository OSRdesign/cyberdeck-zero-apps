/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the TCP port check (no LVGL; tests/test_net.cpp runs it against listeners on loopback).
 *
 * A non-blocking connect() and a poll() with a timeout, one port after the other (a preset may hold several). The
 * result of each port:
 *   Open         the connection was accepted (closed again at once: nothing is sent)
 *   Closed       the host answered with a reset: it is there, nothing listens on that port
 *   Filtered     no answer within the timeout: a firewall drops the packets (or the host is off)
 *   Unreachable  an ICMP "unreachable" or no route: the network says the host cannot be reached
 *   Error        anything else (the socket could not be made)
 * with the connect time (Open / Closed). A name goes through the DNS client (resolve_host).
 */
#pragma once

#include "dns_client.hpp"
#include "netaddr.hpp"
#include "worker.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nt {

enum class PortState { Pending, Open, Closed, Filtered, Unreachable, Error };

/* "open", "closed", "filtered", "unreachable", "error", "..." */
const char *port_state_name(PortState s);

/* errno of the connect (0 = connected) -> the state (pure) */
PortState classify_connect(int err);

struct PortPreset {
    const char *label;             /* "Web", "Mail", "SSH", ... */
    std::vector<std::uint16_t> ports;
};

/* Web 80 443, Mail 25 465 587 993, SSH 22, DNS 53, Remote desktop 3389, VNC 5900, File sharing 445, MQTT 1883,
 * Printer 631 9100, HTTP alt 8080 8443 */
const std::vector<PortPreset> &port_presets();
/* a well-known name for a port ("https"), "" */
const char *port_service(std::uint16_t port);

struct PortResult {
    std::uint16_t port = 0;
    PortState state = PortState::Pending;
    int connect_ms = 0;            /* Open / Closed: the time the answer took */
    int err = 0;                   /* errno */
    std::string detail;            /* "connection refused", "timed out", ... */
};

/* One port of one address, by the timeout (a worker thread). */
PortResult check_port(const Address &addr, std::uint16_t port, int timeout_ms, const CancelToken &cancel);

struct PortRequest {
    std::string host;
    std::vector<std::uint16_t> ports;  /* 1..16 ports, checked in this order */
    int timeout_ms = 3000;
    ResolveOptions resolve;
};

struct PortSnapshot {
    enum class State { Idle, Resolving, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string host;
    std::string address;
    std::string resolved_by;            /* "", "hosts" or the DNS server */
    std::string error;                  /* Failed: why */
    std::vector<PortResult> results;    /* one per port, Pending until checked */
};

class PortSession {
public:
    bool start(const PortRequest &req); /* LVGL thread; a running check is cancelled first */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const PortSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    Channel<PortSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
