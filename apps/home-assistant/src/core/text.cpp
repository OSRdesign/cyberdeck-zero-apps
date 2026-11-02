/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: text helpers (see text.hpp).
 */
#include "text.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace ha {

std::string url_encode(const std::string &s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields)
{
    std::string out;
    for (const auto &f : fields) {
        if (!out.empty()) out += '&';
        out += url_encode(f.first);
        out += '=';
        out += url_encode(f.second);
    }
    return out;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string &query)
{
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string part = query.substr(pos, amp - pos);
        if (!part.empty()) {
            const std::size_t eq = part.find('=');
            const std::string k = url_decode(part.substr(0, eq));
            const std::string v = eq == std::string::npos ? std::string() : url_decode(part.substr(eq + 1));
            if (!k.empty() && !out.count(k)) out[k] = v;
        }
        pos = amp + 1;
    }
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool constant_time_equal(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

std::string hex(const unsigned char *data, std::size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 15];
    }
    return out;
}

std::string clock_hhmm(std::int64_t epoch)
{
    if (epoch <= 0) return "";
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

/* --------------------------------------------------------------------------------------------- the server address */

static std::string host_for_url(const std::string &host)
{
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string Server::base_url() const
{
    std::string u = scheme + "://" + host_for_url(host);
    if (port && port != (scheme == "https" ? 443 : 80)) u += ":" + std::to_string(port);
    return u;
}

std::string Server::base_url_with_host(const std::string &other_host) const
{
    Server s = *this;
    s.host = other_host;
    return s.base_url();
}

std::string Server::websocket_url() const
{
    std::string u = (scheme == "https" ? "wss://" : "ws://") + host_for_url(host);
    if (port && port != (scheme == "https" ? 443 : 80)) u += ":" + std::to_string(port);
    return u + "/api/websocket";
}

std::string Server::display() const
{
    std::string d = host_for_url(host);
    if (port && port != (scheme == "https" ? 443 : 80)) d += ":" + std::to_string(port);
    return scheme == "https" ? "https://" + d : d;
}

bool parse_server(const std::string &text, Server &out, std::string *why)
{
    auto bad = [why](const char *w) {
        if (why) *why = w;
        return false;
    };
    std::string s = trim(text);
    if (s.empty()) return bad("Type the address of Home Assistant");
    Server r;
    const std::size_t sep = s.find("://");
    if (sep != std::string::npos) {
        const std::string scheme = lower(s.substr(0, sep));
        if (scheme != "http" && scheme != "https") return bad("The address must start with http:// or https://");
        r.scheme = scheme;
        s = s.substr(sep + 3);
    }
    /* drop a path ("/", "/lovelace/0") and anything after it */
    const std::size_t slash = s.find_first_of("/?#");
    if (slash != std::string::npos) s = s.substr(0, slash);
    if (s.find('@') != std::string::npos) return bad("No user name in the address, please");
    std::string host, port;
    if (!s.empty() && s[0] == '[') {
        const std::size_t close = s.find(']');
        if (close == std::string::npos) return bad("An IPv6 address needs its closing ]");
        host = s.substr(1, close - 1);
        const std::string rest = s.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return bad("Not an address");
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = s.rfind(':');
        if (colon != std::string::npos && s.find(':') == colon) {
            host = s.substr(0, colon);
            port = s.substr(colon + 1);
        } else if (colon != std::string::npos) {
            host = s; /* a bare IPv6 literal */
        } else {
            host = s;
        }
    }
    if (host.empty()) return bad("Type the address of Home Assistant");
    for (unsigned char c : host)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '%'))
            return bad("The address has a character that cannot be in a host name");
    if (!port.empty()) {
        for (unsigned char c : port)
            if (!std::isdigit(c)) return bad("The port must be a number (8123)");
        const long p = std::strtol(port.c_str(), nullptr, 10);
        if (p < 1 || p > 65535) return bad("The port must be between 1 and 65535");
        r.port = static_cast<int>(p);
    }
    r.host = lower(host);
    out = r;
    return true;
}

bool is_private_ip(const std::string &ip)
{
    unsigned char b[16];
    if (inet_pton(AF_INET, ip.c_str(), b) == 1) {
        if (b[0] == 10 || b[0] == 127) return true;
        if (b[0] == 172 && (b[1] & 0xF0) == 16) return true;
        if (b[0] == 192 && b[1] == 168) return true;
        if (b[0] == 169 && b[1] == 254) return true;
        return false;
    }
    if (inet_pton(AF_INET6, ip.c_str(), b) == 1) {
        bool loop = b[15] == 1;
        for (int i = 0; i < 15; ++i) loop = loop && b[i] == 0;
        if (loop) return true;
        if ((b[0] & 0xFE) == 0xFC) return true;               /* fc00::/7 unique local */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true; /* fe80::/10 link local */
        return false;
    }
    return false;
}

} // namespace ha
