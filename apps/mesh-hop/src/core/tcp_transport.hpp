/*
 * SPDX-License-Identifier: MIT
 *
 * TCP transport to a MeshCore companion with the Wi-Fi (or Ethernet) interface (libc only, no thread except a name
 * lookup). Facts (docs/CONNECT.md): the firmware listens on TCP port 5000 (TCP_PORT, ETHERNET_TCP_PORT) and frames
 * exactly as over USB ('>' + u16 LE length + payload from the board, '<' + u16 LE length + payload to it), so the
 * Client's FrameParser reads the bytes as they come; the board serves ONE client at a time (a new connection replaces
 * the old one) and asks for no password.
 *
 * Non-blocking like the serial transport: open() starts a connect (a numeric address at once; a host name is looked up
 * on a short-lived thread first) and returns NotFound until the connection is up; poll() (the app's timer) and every
 * open() move it on; connect_timeout_ms (5 s) bounds a connect to an absent host. read() returns -1 when the board
 * closes the connection or the socket fails (keepalive: an idle dead link is noticed in about 25 s).
 */

#pragma once

#include "transport.hpp"

#include <memory>

namespace meshzero {

/* The companion firmware's default TCP port (examples/companion_radio/main.cpp TCP_PORT). */
constexpr int kMeshcoreTcpPort = 5000;

class TcpTransport : public ITransport {
public:
    TcpTransport(std::string host, int port, std::string name = "", int connect_timeout_ms = 5000);
    ~TcpTransport() override;

    OpenStatus open() override;
    void close() override;
    bool is_open() const override { return state_ == State::Open; }
    long read(uint8_t *buf, size_t max) override;
    bool write(const uint8_t *data, size_t len) override;
    std::string kind() const override { return "Network (TCP)"; }
    const LinkInfo &info() const override { return info_; }
    std::string last_error() const override { return error_; }

    /* Moves a connect in progress on (the app calls it from its timer; open() calls it too). */
    void poll() override;
    bool connecting() const { return state_ == State::Resolving || state_ == State::Connecting; }
    /* The reason of the last failed connect ("" once connected): the start page keeps showing it while the Client tries again. */
    const std::string &last_failure() const { return failure_; }
    const std::string &host() const { return host_; }
    int port() const { return port_; }

    struct Resolve;   // a name lookup in flight (shared with its thread)

private:
    enum class State { Idle, Resolving, Connecting, Open, Failed };
    void start_connect(const void *addr, unsigned addr_len, int family);
    void fail(const std::string &why, OpenStatus status);
    void flush_tx();

    std::string host_;
    int port_;
    int timeout_ms_;
    State state_ = State::Idle;
    OpenStatus fail_status_ = OpenStatus::NotFound;
    int fd_ = -1;
    uint64_t deadline_ = 0;
    std::shared_ptr<Resolve> resolve_;
    LinkInfo info_;
    std::string error_;
    std::string failure_;
    std::string tx_;
    bool lost_ = false;
};

/* A transport that never opens: the app before the user chose a node (no auto-connect). */
class NullTransport : public ITransport {
public:
    OpenStatus open() override { return OpenStatus::NotFound; }
    void close() override {}
    bool is_open() const override { return false; }
    long read(uint8_t *, size_t) override { return -1; }
    bool write(const uint8_t *, size_t) override { return false; }
    std::string kind() const override { return "none"; }
    const LinkInfo &info() const override { return info_; }
    std::string last_error() const override { return "No node chosen"; }

private:
    LinkInfo info_;
};

/* host is a numeric IPv4 / IPv6 address or a host name of letters, digits, dots and dashes (no spaces, at most 253). */
bool valid_host(const std::string &host);

} // namespace meshzero
