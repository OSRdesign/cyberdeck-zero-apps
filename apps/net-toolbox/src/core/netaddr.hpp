/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: IP addresses and host names (no LVGL, no network; tests/test_parsers.cpp).
 */
#pragma once

#include <cstdint>
#include <string>

#include <sys/socket.h>

namespace nt {

struct Address {
    int family = 0;              /* AF_INET, AF_INET6; 0 = none */
    std::uint8_t bytes[16] = {}; /* network order; 4 used for IPv4 */
    std::uint32_t scope = 0;     /* IPv6 scope id (fe80::1%wlan0) */

    bool valid() const { return family == AF_INET || family == AF_INET6; }
    std::string text() const;    /* "192.168.68.1", "fe80::1%wlan0"; "" when not valid */
    /* a sockaddr_in / sockaddr_in6 with `port`; returns its length (0 when not valid) */
    socklen_t to_sockaddr(std::uint16_t port, sockaddr_storage &out) const;
    static Address from_sockaddr(const sockaddr *sa);
    bool operator==(const Address &o) const;
    bool operator!=(const Address &o) const { return !(*this == o); }
};

/* A literal IPv4 or IPv6 address ("192.168.1.1", "2606:4700::1111", "fe80::1%wlan0"); false for anything else (a
 * name). An IPv4 address needs its four dotted parts (inet_pton rules). */
bool parse_ip(const std::string &text, Address &out);
inline bool is_ip(const std::string &text)
{
    Address a;
    return parse_ip(text, a);
}

/* 169.254.0.0/16 (self-assigned when DHCP did not answer) */
bool is_ipv4_link_local(const Address &a);
bool is_loopback(const Address &a);

/* The name to ask a PTR record for: "4.3.2.1.in-addr.arpa", nibbles + ".ip6.arpa"; "" when not an address. */
std::string reverse_name(const Address &a);

/* A host name we may put in a query: letters, digits, '-', '_', dots between labels of 1..63 bytes, at most 253 bytes,
 * one trailing dot allowed. Error words for the user in *why. */
bool valid_hostname(const std::string &name, std::string *why = nullptr);

/* A host the user typed: an address or a valid name ("" and spaces refused). */
bool valid_host(const std::string &text, std::string *why = nullptr);

} // namespace nt
