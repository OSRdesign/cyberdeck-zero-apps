/*
 * SPDX-License-Identifier: MIT
 */

#include "relay.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace meshzero {

RelayServer::~RelayServer()
{
    close();
}

int RelayServer::open()
{
    close();
    error_.clear();
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error_ = std::string("socket: ") + std::strerror(errno);
        return 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;                                         // the kernel picks a free port
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);             // 127.0.0.1 only: not reachable from the network
    socklen_t len = sizeof(a);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || ::listen(fd, 1) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) != 0) {
        error_ = std::string("listen on 127.0.0.1: ") + std::strerror(errno);
        ::close(fd);
        return 0;
    }
    listen_fd_ = fd;
    port_ = ntohs(a.sin_port);
    return port_;
}

void RelayServer::close()
{
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    drop_client();
    port_ = 0;
}

void RelayServer::drop_client()
{
    if (client_fd_ >= 0) ::close(client_fd_);
    client_fd_ = -1;
    out_.clear();
    unframer_.reset();
}

void RelayServer::send(const Bytes &payload)
{
    if (client_fd_ < 0 || payload.empty()) return;
    const Bytes f = ble::wrap_from_device(payload);
    out_.insert(out_.end(), f.begin(), f.end());
}

void RelayServer::flush()
{
    while (client_fd_ >= 0 && !out_.empty()) {
        const ssize_t n = ::send(client_fd_, out_.data(), out_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(out_.begin(), out_.begin() + n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        gone_ = true;                                       // EPIPE, ECONNRESET: the client went away
        return;
    }
    if (out_.size() > kMaxPendingOut) gone_ = true;        // it stopped reading
}

RelayServer::Io RelayServer::poll(std::vector<Bytes> &frames)
{
    if (client_fd_ < 0 && listen_fd_ >= 0) {
        const int c = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) {
            client_fd_ = c;
            const int one = 1;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            ::close(listen_fd_);                            // one client per session: nobody else gets in
            listen_fd_ = -1;
        }
    }
    if (client_fd_ < 0) return Io::Idle;
    gone_ = false;
    flush();
    uint8_t buf[1024];
    for (int i = 0; i < 8 && !gone_; ++i) {
        const ssize_t n = ::recv(client_fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            unframer_.feed(buf, static_cast<size_t>(n), frames);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        gone_ = true;                                       // 0: an orderly close; < 0: an error
    }
    if (gone_) {
        drop_client();
        return Io::Gone;
    }
    return Io::Ok;
}

} // namespace meshzero
