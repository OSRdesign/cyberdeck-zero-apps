/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: IP addresses and host names (see netaddr.hpp).
 */
#include "netaddr.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstring>

namespace nt {

std::string Address::text() const
{
    char buf[INET6_ADDRSTRLEN + IF_NAMESIZE + 2] = {};
    if (family == AF_INET) {
        if (!inet_ntop(AF_INET, bytes, buf, sizeof(buf))) return std::string();
        return buf;
    }
    if (family == AF_INET6) {
        if (!inet_ntop(AF_INET6, bytes, buf, INET6_ADDRSTRLEN)) return std::string();
        std::string out = buf;
        if (scope) {
            char name[IF_NAMESIZE] = {};
            out += '%';
            out += if_indextoname(scope, name) ? std::string(name) : std::to_string(scope);
        }
        return out;
    }
    return std::string();
}

socklen_t Address::to_sockaddr(std::uint16_t port, sockaddr_storage &out) const
{
    std::memset(&out, 0, sizeof(out));
    if (family == AF_INET) {
        auto *sin = reinterpret_cast<sockaddr_in *>(&out);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        std::memcpy(&sin->sin_addr, bytes, 4);
        return sizeof(sockaddr_in);
    }
    if (family == AF_INET6) {
        auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&out);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        std::memcpy(&sin6->sin6_addr, bytes, 16);
        sin6->sin6_scope_id = scope;
        return sizeof(sockaddr_in6);
    }
    return 0;
}

Address Address::from_sockaddr(const sockaddr *sa)
{
    Address a;
    if (!sa) return a;
    if (sa->sa_family == AF_INET) {
        const auto *sin = reinterpret_cast<const sockaddr_in *>(sa);
        a.family = AF_INET;
        std::memcpy(a.bytes, &sin->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
        a.family = AF_INET6;
        std::memcpy(a.bytes, &sin6->sin6_addr, 16);
        a.scope = sin6->sin6_scope_id;
    }
    return a;
}

bool Address::operator==(const Address &o) const
{
    if (family != o.family) return false;
    if (family == AF_INET) return std::memcmp(bytes, o.bytes, 4) == 0;
    if (family == AF_INET6) return std::memcmp(bytes, o.bytes, 16) == 0 && scope == o.scope;
    return true;
}

bool parse_ip(const std::string &text, Address &out)
{
    out = Address();
    if (text.empty() || text.size() > 64) return false;
    if (text.find(':') == std::string::npos) {
        in_addr v4;
        if (inet_pton(AF_INET, text.c_str(), &v4) != 1) return false;
        out.family = AF_INET;
        std::memcpy(out.bytes, &v4, 4);
        return true;
    }
    std::string host = text;
    std::string zone;
    const std::size_t pct = host.find('%');
    if (pct != std::string::npos) {
        zone = host.substr(pct + 1);
        host.erase(pct);
        if (zone.empty()) return false;
    }
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    in6_addr v6;
    if (inet_pton(AF_INET6, host.c_str(), &v6) != 1) return false;
    out.family = AF_INET6;
    std::memcpy(out.bytes, &v6, 16);
    if (!zone.empty()) {
        bool digits = true;
        for (const char c : zone)
            if (c < '0' || c > '9') digits = false;
        if (digits && zone.size() < 10) out.scope = static_cast<std::uint32_t>(std::strtoul(zone.c_str(), nullptr, 10));
        else out.scope = if_nametoindex(zone.c_str());
        if (!out.scope) {
            out = Address();
            return false; /* an unknown interface: the address cannot be used */
        }
    }
    return true;
}

bool is_ipv4_link_local(const Address &a)
{
    return a.family == AF_INET && a.bytes[0] == 169 && a.bytes[1] == 254;
}

bool is_loopback(const Address &a)
{
    if (a.family == AF_INET) return a.bytes[0] == 127;
    if (a.family == AF_INET6) {
        for (int i = 0; i < 15; ++i)
            if (a.bytes[i]) return false;
        return a.bytes[15] == 1;
    }
    return false;
}

std::string reverse_name(const Address &a)
{
    char buf[80];
    if (a.family == AF_INET) {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u.in-addr.arpa", a.bytes[3], a.bytes[2], a.bytes[1], a.bytes[0]);
        return buf;
    }
    if (a.family == AF_INET6) {
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (int i = 15; i >= 0; --i) {
            out += hex[a.bytes[i] & 0xF];
            out += '.';
            out += hex[a.bytes[i] >> 4];
            out += '.';
        }
        return out + "ip6.arpa";
    }
    return std::string();
}

bool valid_hostname(const std::string &input, std::string *why)
{
    const auto fail = [&](const char *text) {
        if (why) *why = text;
        return false;
    };
    std::string name = input;
    if (!name.empty() && name.back() == '.') name.pop_back();
    if (name.empty()) return fail("Type a host name or an address");
    if (name.size() > 253) return fail("Name too long (253 characters at most)");
    std::size_t label = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '.') {
            if (label == 0) return fail("Empty part between dots");
            label = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (!ok) return fail(c == ' ' ? "No spaces in a host name" : "Only letters, digits, '-' and '.' in a name");
        if (++label > 63) return fail("A part of the name is longer than 63 characters");
    }
    if (label == 0) return fail("Empty part between dots");
    return true;
}

bool valid_host(const std::string &text, std::string *why)
{
    if (is_ip(text)) return true;
    return valid_hostname(text, why);
}

} // namespace nt
