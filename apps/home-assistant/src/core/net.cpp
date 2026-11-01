/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the network pieces of the connection thread (see net.hpp).
 */
#include "net.hpp"

#include "curl_api.hpp"
#include "json.hpp"
#include "text.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace ha {

namespace {

constexpr int kRcvBuf = 64 * 1024;
constexpr std::size_t kChunk = 64 * 1024;
constexpr std::size_t kMaxHttpBody = 256 * 1024;

int sockopt_cb(void *, int fd, int)
{
    int v = kRcvBuf;
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0 ? curl::SOCKOPT_OK : curl::SOCKOPT_ERROR;
}

int xferinfo_cb(void *user, curl::off_t64, curl::off_t64, curl::off_t64, curl::off_t64)
{
    const auto *abort = static_cast<const std::atomic<bool> *>(user);
    return abort && abort->load() ? 1 : 0;
}

struct Body {
    std::string text;
};

std::size_t write_cb(char *ptr, std::size_t size, std::size_t nmemb, void *user)
{
    auto *b = static_cast<Body *>(user);
    const std::size_t n = size * nmemb;
    if (b->text.size() + n > kMaxHttpBody) return 0; /* too large: abort */
    b->text.append(ptr, n);
    return n;
}

void common_options(void *h, const std::atomic<bool> *abort)
{
    const curl::Api &c = curl::api();
    c.easy_setopt(h, curl::OPT_NOSIGNAL, 1L);
    c.easy_setopt(h, curl::OPT_NOPROXY, "*");
    c.easy_setopt(h, curl::OPT_IPRESOLVE, curl::IPRESOLVE_V4);
    c.easy_setopt(h, curl::OPT_USERAGENT, "CyberdeckZero-HomeAssistant/0.1");
    c.easy_setopt(h, curl::OPT_NOPROGRESS, 0L);
    c.easy_setopt(h, curl::OPT_XFERINFOFUNCTION, &xferinfo_cb);
    c.easy_setopt(h, curl::OPT_XFERINFODATA, const_cast<std::atomic<bool> *>(abort));
}

} // namespace

/* --------------------------------------------------------------------------------------------- WebSocket */

bool WsLink::open(const std::string &url, int connect_ms, const std::atomic<bool> *abort, std::string &why)
{
    close();
    if (!curl::load(why)) return false;
    const curl::Api &c = curl::api();
    void *h = c.easy_init();
    if (!h) {
        why = "libcurl could not start a connection";
        return false;
    }
    common_options(h, abort);
    c.easy_setopt(h, curl::OPT_URL, url.c_str());
    c.easy_setopt(h, curl::OPT_CONNECT_ONLY, 2L);
    c.easy_setopt(h, curl::OPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_ms));
    c.easy_setopt(h, curl::OPT_SOCKOPTFUNCTION, &sockopt_cb);
    c.easy_setopt(h, curl::OPT_SOCKOPTDATA, static_cast<void *>(nullptr));
    c.easy_setopt(h, curl::OPT_TCP_KEEPALIVE, 1L);
    c.easy_setopt(h, curl::OPT_TCP_KEEPIDLE, 20L);
    c.easy_setopt(h, curl::OPT_TCP_KEEPINTVL, 10L);
    const curl::Code rc = c.easy_perform(h);
    if (rc != curl::OK) {
        why = curl::strerror(rc);
        c.easy_cleanup(h);
        return false;
    }
    long sock = -1;
    int sockfd = -1;
    /* curl_socket_t is an int on Linux */
    if (c.easy_getinfo(h, curl::INFO_ACTIVESOCKET, &sockfd) != curl::OK || sockfd < 0) {
        (void)sock;
        why = "no socket after the connection";
        c.easy_cleanup(h);
        return false;
    }
    handle_ = h;
    fd_ = sockfd;
    partial_.clear();
    skipping_ = false;
    buf_.resize(kChunk);
    return true;
}

bool WsLink::read(std::vector<std::string> &out, std::string &why)
{
    if (!handle_) {
        why = "not connected";
        return false;
    }
    const curl::Api &c = curl::api();
    for (int guard = 0; guard < 4096; ++guard) {
        std::size_t n = 0;
        const curl::WsFrame *meta = nullptr;
        const curl::Code rc = c.ws_recv(handle_, buf_.data(), buf_.size(), &n, &meta);
        if (rc == curl::AGAIN) return true;
        if (rc != curl::OK) {
            why = rc == curl::GOT_NOTHING ? "Home Assistant closed the connection" : curl::strerror(rc);
            return false;
        }
        if (!meta) continue;
        if (meta->flags & (curl::WS_PING | curl::WS_PONG)) continue; /* libcurl answered the ping itself */
        if (meta->flags & curl::WS_CLOSE) {
            why = "Home Assistant closed the connection";
            return false;
        }
        if (!skipping_) {
            if (partial_.size() + n > json::kMaxText) {
                skipping_ = true;
                partial_.clear();
                partial_.shrink_to_fit();
            } else {
                partial_.append(buf_.data(), n);
            }
        }
        if (meta->bytesleft == 0 && !(meta->flags & curl::WS_CONT)) {
            if (skipping_) {
                ++dropped_;
                std::fprintf(stderr, "[home-assistant] a message over %zu bytes was dropped\n", json::kMaxText);
            } else {
                if (partial_.size() > largest_) largest_ = partial_.size();
                out.push_back(std::move(partial_));
            }
            partial_.clear();
            if (partial_.capacity() > 2 * kChunk) partial_.shrink_to_fit();
            skipping_ = false;
        }
    }
    return true;
}

bool WsLink::send(const std::string &text, std::string &why)
{
    if (!handle_) {
        why = "not connected";
        return false;
    }
    const curl::Api &c = curl::api();
    std::size_t off = 0;
    int waits = 0;
    while (off < text.size()) {
        std::size_t sent = 0;
        const curl::Code rc = c.ws_send(handle_, text.data() + off, text.size() - off, &sent, 0, curl::WS_TEXT);
        if (rc == curl::AGAIN) {
            if (++waits > 50) {
                why = "Home Assistant does not take what the deck sends";
                return false;
            }
            struct pollfd p = {fd_, POLLOUT, 0};
            ::poll(&p, 1, 100);
            continue;
        }
        if (rc != curl::OK) {
            why = curl::strerror(rc);
            return false;
        }
        off += sent;
        if (sent == 0 && ++waits > 50) {
            why = "the send did not progress";
            return false;
        }
    }
    return true;
}

void WsLink::close()
{
    if (handle_) curl::api().easy_cleanup(handle_);
    handle_ = nullptr;
    fd_ = -1;
    partial_.clear();
    partial_.shrink_to_fit();
    skipping_ = false;
}

/* --------------------------------------------------------------------------------------------- HTTP */

HttpResult http_request(const std::string &url, const std::string *post_body, int timeout_ms,
                        const std::atomic<bool> *abort)
{
    HttpResult r;
    if (!curl::load(r.error)) return r;
    const curl::Api &c = curl::api();
    void *h = c.easy_init();
    if (!h) {
        r.error = "libcurl could not start a request";
        return r;
    }
    Body body;
    common_options(h, abort);
    c.easy_setopt(h, curl::OPT_URL, url.c_str());
    c.easy_setopt(h, curl::OPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    c.easy_setopt(h, curl::OPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms < 5000 ? timeout_ms : 5000));
    c.easy_setopt(h, curl::OPT_WRITEFUNCTION, &write_cb);
    c.easy_setopt(h, curl::OPT_WRITEDATA, static_cast<void *>(&body));
    if (post_body) {
        c.easy_setopt(h, curl::OPT_POST, 1L);
        c.easy_setopt(h, curl::OPT_POSTFIELDSIZE, static_cast<long>(post_body->size()));
        c.easy_setopt(h, curl::OPT_POSTFIELDS, post_body->c_str()); /* form-urlencoded by default */
    } else {
        c.easy_setopt(h, curl::OPT_HTTPGET, 1L);
    }
    const curl::Code rc = c.easy_perform(h);
    if (rc == curl::OK) {
        long code = 0;
        c.easy_getinfo(h, curl::INFO_RESPONSE_CODE, &code);
        r.status = static_cast<int>(code);
        r.body = std::move(body.text);
    } else {
        r.error = curl::strerror(rc);
    }
    c.easy_cleanup(h);
    return r;
}

/* --------------------------------------------------------------------------------------------- listener */

bool Listener::start(std::string &why)
{
    stop();
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        why = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int want = 0;
    if (const char *p = std::getenv("HA_LISTEN_PORT")) want = std::atoi(p);
    a.sin_port = htons(static_cast<uint16_t>(want > 0 && want < 65536 ? want : 0));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd, 4) != 0) {
        why = std::string("cannot open a port: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    fd_ = fd;
    port_ = ntohs(a.sin_port);
    return true;
}

void Listener::stop()
{
    if (client_ >= 0) ::close(client_);
    client_ = -1;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    port_ = 0;
}

bool Listener::accept_request(std::string &path, std::map<std::string, std::string> &query, std::string &peer)
{
    if (fd_ < 0) return false;
    sockaddr_in from{};
    socklen_t len = sizeof from;
    const int c = ::accept4(fd_, reinterpret_cast<sockaddr *>(&from), &len, SOCK_CLOEXEC);
    if (c < 0) return false;
    char ip[INET_ADDRSTRLEN] = "";
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    peer = ip;
    struct timeval tv = {3, 0};
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        const ssize_t n = ::recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
    }
    const std::size_t eol = req.find("\r\n");
    const std::string line = req.substr(0, eol);
    /* "GET /callback?code=...&state=... HTTP/1.1" */
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (line.compare(0, 4, "GET ") != 0 || sp1 == std::string::npos || sp2 == std::string::npos) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)::send(c, bad, std::strlen(bad), MSG_NOSIGNAL);
        ::close(c);
        return false;
    }
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = target.find('?');
    path = target.substr(0, q);
    query = q == std::string::npos ? std::map<std::string, std::string>() : parse_query(target.substr(q + 1));
    if (client_ >= 0) ::close(client_);
    client_ = c;
    return true;
}

void Listener::respond(int status, const std::string &html_body)
{
    if (client_ < 0) return;
    const std::string page = "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
                             "content=\"width=device-width\"><title>Home Assistant on the deck</title></head>"
                             "<body style=\"font-family:sans-serif;font-size:140%;padding:2em\">" +
                             html_body + "</body></html>";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
                             "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                             "Content-Length: " +
                             std::to_string(page.size()) + "\r\nConnection: close\r\n\r\n";
    const std::string all = head + page;
    (void)::send(client_, all.data(), all.size(), MSG_NOSIGNAL);
    ::close(client_);
    client_ = -1;
}

/* --------------------------------------------------------------------------------------------- addresses */

bool lan_address_towards(const std::string &host, int port, std::string &lan_ip, std::string &host_ip,
                         std::string &why)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        why = "cannot find " + host + " on the network (" + gai_strerror(rc) + ")";
        return false;
    }
    char ip[INET_ADDRSTRLEN] = "";
    ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr, ip, sizeof ip);
    host_ip = ip;
    const int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool ok = false;
    if (s >= 0 && ::connect(s, res->ai_addr, res->ai_addrlen) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (::getsockname(s, reinterpret_cast<sockaddr *>(&me), &len) == 0) {
            char mine[INET_ADDRSTRLEN] = "";
            ::inet_ntop(AF_INET, &me.sin_addr, mine, sizeof mine);
            lan_ip = mine;
            ok = true;
        }
    }
    if (!ok) why = std::string("no route to ") + host + ": " + std::strerror(errno);
    if (s >= 0) ::close(s);
    ::freeaddrinfo(res);
    return ok;
}

std::string random_state()
{
    unsigned char b[16] = {};
    bool ok = false;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ok = ::read(fd, b, sizeof b) == static_cast<ssize_t>(sizeof b);
        ::close(fd);
    }
    if (!ok) return std::string(); /* the engine refuses a short state */
    return hex(b, sizeof b);
}

} // namespace ha
