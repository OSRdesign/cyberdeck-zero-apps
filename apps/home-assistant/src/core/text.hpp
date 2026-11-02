/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: text helpers (URL and form encoding, query strings, the server address, clock text). No
 * LVGL; tests/test_text.cpp.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ha {

/* RFC 3986 percent-encoding: unreserved characters (A-Z a-z 0-9 - . _ ~) kept, every other byte %XX. */
std::string url_encode(const std::string &s);
/* application/x-www-form-urlencoded: name=value pairs joined with &, each side url_encode()d. */
std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields);
/* %XX decoded, '+' as a space (form and query strings); a malformed escape is kept as it is. */
std::string url_decode(const std::string &s);
/* "a=1&b=x%20y" -> {a: 1, b: "x y"}; the first value of a repeated name wins. */
std::map<std::string, std::string> parse_query(const std::string &query);

std::string lower(std::string s);
std::string trim(const std::string &s);
/* Same length and bytes, in time that does not depend on where they differ (the pairing's state). */
bool constant_time_equal(const std::string &a, const std::string &b);
/* Lowercase hex of bytes (random state strings). */
std::string hex(const unsigned char *data, std::size_t n);

/* The local time of an epoch as "HH:MM" ("" for 0). */
std::string clock_hhmm(std::int64_t epoch);

/* --------------------------------------------------------------------------------------------- the server address */

/* What the user types: "homeassistant.local:8123", "192.168.1.5", "http://ha.lan:8123/", "https://ha.example",
 * "[fd00::5]:8123". No port: the scheme's own (80 / 443), as a browser would; HA OS listens on port 80 since 2026.8 and
 * a Container install on 8123, so the default address of the app names 8123 explicitly. */
struct Server {
    std::string scheme = "http"; /* http or https */
    std::string host;            /* without brackets for an IPv6 literal */
    int port = 0;                /* 0: the scheme's default */
    bool valid() const { return !host.empty(); }
    int effective_port() const { return port ? port : (scheme == "https" ? 443 : 80); }
    /* "http://homeassistant.local:8123" (no trailing slash; the default port left out) */
    std::string base_url() const;
    /* "ws://homeassistant.local:8123/api/websocket" (wss for https) */
    std::string websocket_url() const;
    /* What the rows show: "homeassistant.local:8123" (the scheme only when it is https) */
    std::string display() const;
    /* The base URL with another host (the QR code's IP address form) */
    std::string base_url_with_host(const std::string &other_host) const;
};

constexpr const char *kDefaultServer = "homeassistant.local:8123";

/* false with a reason in plain words when the text is not an address. */
bool parse_server(const std::string &text, Server &out, std::string *why = nullptr);

/* An IPv4 or IPv6 literal on a private, loopback or link-local network (what Home Assistant accepts in a client id). */
bool is_private_ip(const std::string &ip);

} // namespace ha
