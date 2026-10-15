/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: a minimal plain-HTTP GET for the captive-portal check (no LVGL, no TLS; tests/test_net.cpp runs it
 * against a listener on loopback). It sends one HTTP/1.1 request with "Connection: close", reads the status line and
 * the headers (at most 8 kB) and stops: the body is never read.
 */
#pragma once

#include "netaddr.hpp"
#include "worker.hpp"

#include <cstdint>
#include <string>

namespace nt {

struct HttpResult {
    bool answered = false;   /* a status line arrived */
    int status = 0;          /* 204, 200, 302 ... */
    std::string location;    /* the Location header of a redirect */
    int err = 0;             /* errno when not answered */
    std::string error;       /* plain words when not answered */
    int elapsed_ms = 0;
};

/* The status line and the Location header of a response head (pure). False when it is not HTTP. */
bool parse_http_head(const std::string &head, int &status, std::string &location);

/* GET path from addr:port with "Host: host", by the timeout. */
HttpResult http_get(const Address &addr, std::uint16_t port, const std::string &host, const std::string &path,
                    int timeout_ms, const CancelToken &cancel);

} // namespace nt
