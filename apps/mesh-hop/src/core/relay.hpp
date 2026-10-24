/*
 * SPDX-License-Identifier: MIT
 *
 * The loopback end of the CLI relay (report 033, option (b)): a TCP listener on 127.0.0.1 that a meshcore-cli session
 * (`meshcli -t 127.0.0.1 -p <port>`) connects to. It speaks the framing of the companion's TCP interface, which is the USB
 * serial framing (meshcore_py tcp_cx.py): the client sends '<' + u16 LE length + payload, it receives '>' + u16 LE length +
 * payload. This class only moves bytes: what the frames mean, and when they may go to the node, is the Client's business
 * (client.hpp, "the CLI relay").
 *
 * Rules: the loopback address only; an ephemeral port chosen by the kernel; ONE client: the listener is closed as soon as it
 * is accepted, so nobody else can connect for the rest of the session. libc only, non-blocking, no thread: poll() from the
 * Client's poll().
 */

#pragma once

#include "ble/ble_framing.hpp"
#include "util.hpp"

#include <string>
#include <vector>

namespace meshzero {

class RelayServer {
public:
    /* What the client may leave unread before it counts as stalled (its session is then ended). */
    static constexpr size_t kMaxPendingOut = 256 * 1024;

    RelayServer() = default;
    ~RelayServer();
    RelayServer(const RelayServer &) = delete;
    RelayServer &operator=(const RelayServer &) = delete;

    /* Binds 127.0.0.1 on a port the kernel picks and listens for one client. Returns the port, or 0 (error() says why). */
    int open();
    /* Closes the listener and the client. */
    void close();

    bool listening() const { return listen_fd_ >= 0; }
    bool connected() const { return client_fd_ >= 0; }
    int port() const { return port_; }
    const std::string &error() const { return error_; }

    enum class Io { Idle, Ok, Gone };
    /* Accepts the client when it knocks (then the listener closes), sends what is queued for it and reads what it sent: the
     * payloads of its complete frames are appended to frames. Gone: the client closed the connection, failed, or stopped
     * reading (more than kMaxPendingOut queued); the connection is closed then. Idle: no client (yet). */
    Io poll(std::vector<Bytes> &frames);
    /* Queues one frame of the node for the client ('>' + length + payload). Nothing without a client. */
    void send(const Bytes &payload);

    size_t pending_out() const { return out_.size(); }
    size_t bad_bytes() const { return unframer_.bad(); }      // bytes of the client that were not part of a frame

private:
    void drop_client();
    void flush();

    int listen_fd_ = -1;
    int client_fd_ = -1;
    int port_ = 0;
    std::string error_;
    ble::ToDeviceUnframer unframer_;       // '<' + u16 LE length + payload, the frames a companion takes
    Bytes out_;
    bool gone_ = false;
};

} // namespace meshzero
