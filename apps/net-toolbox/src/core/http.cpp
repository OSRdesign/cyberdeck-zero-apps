/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the minimal HTTP GET (see http.hpp).
 */
#include "http.hpp"

#include "sock.hpp"
#include "text.hpp"

#include <algorithm>
#include <cerrno>

namespace nt {

bool parse_http_head(const std::string &head, int &status, std::string &location)
{
    status = 0;
    location.clear();
    const std::vector<std::string> lines = split_lines(head);
    if (lines.empty()) return false;
    const std::vector<std::string> first = split_ws(lines[0]);
    if (first.size() < 2 || !starts_with(first[0], "HTTP/1.")) return false;
    int code = 0;
    if (first[1].size() != 3 || !parse_int(first[1], code) || code < 100 || code > 599) return false;
    status = code;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) break;
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos) continue;
        if (lower(trim(lines[i].substr(0, colon))) == "location") {
            location = trim(lines[i].substr(colon + 1));
            if (location.size() > 512) location.resize(512);
        }
    }
    return true;
}

HttpResult http_get(const Address &addr, std::uint16_t port, const std::string &host, const std::string &path,
                    int timeout_ms, const CancelToken &cancel)
{
    HttpResult r;
    const std::int64_t start = now_ms();
    const std::int64_t deadline = start + std::max(100, timeout_ms);
    const auto done = [&](int e) {
        r.err = e;
        r.error = errno_words(e);
        r.elapsed_ms = static_cast<int>(now_ms() - start);
        return r;
    };
    int e = 0;
    Fd fd = tcp_connect(addr, port, deadline, cancel, &e);
    if (!fd.valid()) return done(e);
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                                "\r\nUser-Agent: net-toolbox\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    if ((e = send_all(fd.get(), request.data(), request.size(), deadline, cancel))) return done(e);
    std::string head;
    char buf[1024];
    while (head.size() < 8192) {
        std::size_t got = 0;
        if ((e = recv_some(fd.get(), buf, sizeof(buf), &got, deadline, cancel))) {
            if (head.empty()) return done(e);
            break; /* what came is enough to read the status */
        }
        if (!got) break;
        head.append(buf, got);
        if (head.find("\r\n\r\n") != std::string::npos || head.find("\n\n") != std::string::npos) break;
    }
    r.elapsed_ms = static_cast<int>(now_ms() - start);
    if (head.empty()) {
        r.err = ECONNRESET;
        r.error = "the connection closed without an answer";
        return r;
    }
    if (!parse_http_head(head, r.status, r.location)) {
        r.err = EPROTO;
        r.error = "an answer that is not HTTP";
        return r;
    }
    r.answered = true;
    return r;
}

} // namespace nt
