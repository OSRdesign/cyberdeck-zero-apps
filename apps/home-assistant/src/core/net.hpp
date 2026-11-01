/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the network pieces of the connection thread (no LVGL). Exercised against the fake Home
 * Assistant of tests/fake_ha (tests/integration).
 *
 *   WsLink      the WebSocket over libcurl (CURLOPT_CONNECT_ONLY = 2, curl_ws_recv / curl_ws_send). SO_RCVBUF is set
 *               to 64 KB before connecting (spike probe 2: libcurl reads whatever the kernel holds into its own heap, a
 *               3 MB message cost 3 MB; with 64 KB it cost nothing). Messages are put back together here (libcurl
 *               hands out frames and parts of frames; the end of a message is bytesleft 0 without CURLWS_CONT), at
 *               most json::kMaxText bytes: a longer one is dropped and named in the log. libcurl answers Home
 *               Assistant's pings by itself, but only from inside curl_ws_recv: the thread reads the socket all the
 *               time. A dead peer shows on the receive side only (a send after a FIN still "works"): the engine's
 *               ping does the rest. TCP keepalive (20 s idle, 10 s apart) is a second net. IPv4 only: Home Assistant
 *               publishes an IPv6 link-local address without a zone that a board may try first.
 *   http_request  one GET or form POST (the token endpoints), 10 s at most, no proxy.
 *   Listener    the pairing listener: one port on every address, a request read in at most 3 s, one line of HTML
 *               back. It answers only while the Connect page waits for the phone.
 *   lan_address_towards  the deck's address on the network that reaches Home Assistant (a UDP "connect" sends nothing).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ha {

class WsLink {
public:
    WsLink() = default;
    ~WsLink() { close(); }
    WsLink(const WsLink &) = delete;
    WsLink &operator=(const WsLink &) = delete;

    /* Connects and upgrades (blocking, connect_ms at most; abort set = give up). */
    bool open(const std::string &url, int connect_ms, const std::atomic<bool> *abort, std::string &why);
    /* Reads what is ready, appends the complete text messages. False: the link is dead (why). */
    bool read(std::vector<std::string> &out, std::string &why);
    bool send(const std::string &text, std::string &why);
    void close();
    bool is_open() const { return handle_ != nullptr; }
    int fd() const { return fd_; }
    std::size_t largest_message() const { return largest_; }
    std::size_t dropped_messages() const { return dropped_; }

private:
    void *handle_ = nullptr;
    int fd_ = -1;
    std::string partial_;
    bool skipping_ = false;
    std::vector<char> buf_;
    std::size_t largest_ = 0, dropped_ = 0;
};

struct HttpResult {
    int status = 0;     /* 0: no answer */
    std::string body;   /* at most 256 KB */
    std::string error;  /* why there was no answer */
};

/* post_body null: GET. */
HttpResult http_request(const std::string &url, const std::string *post_body, int timeout_ms,
                        const std::atomic<bool> *abort);

class Listener {
public:
    ~Listener() { stop(); }
    bool start(std::string &why); /* any free port (HA_LISTEN_PORT picks one, tests) */
    void stop();
    bool running() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    int port() const { return port_; }
    /* Accepts one connection and reads its request line. True with the path and the decoded query; the connection
     * stays open for respond(). */
    bool accept_request(std::string &path, std::map<std::string, std::string> &query, std::string &peer);
    void respond(int status, const std::string &html_body);

private:
    int fd_ = -1;
    int port_ = 0;
    int client_ = -1;
};

/* host's IPv4 address (resolved) and the deck's address that reaches it. */
bool lan_address_towards(const std::string &host, int port, std::string &lan_ip, std::string &host_ip,
                         std::string &why);

/* 16 random bytes as 32 hex characters (/dev/urandom). */
std::string random_state();

} // namespace ha
