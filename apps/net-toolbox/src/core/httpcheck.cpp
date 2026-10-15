/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the HTTP check (see httpcheck.hpp).
 */
#include "httpcheck.hpp"

#include "minijson.hpp"
#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nt {
namespace {

bool contains(const std::string &hay, const char *needle)
{
    return hay.find(needle) != std::string::npos;
}

/* days since 1970-01-01 of a civil date (proleptic Gregorian) */
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool parse_hms(const std::string &s, int &h, int &mi, int &se)
{
    if (s.size() < 8 || s[2] != ':' || s[5] != ':') return false;
    return parse_int(s.substr(0, 2), h) && parse_int(s.substr(3, 2), mi) && parse_int(s.substr(6, 2), se) && h >= 0 &&
           h < 24 && mi >= 0 && mi < 60 && se >= 0 && se <= 60;
}

std::string after_colon(const std::string &line, std::size_t prefix_len)
{
    return trim(line.substr(prefix_len));
}

/* "curl: (60) SSL certificate problem: ..." -> the message (and the code) */
bool curl_error_line(const std::string &output, int &code, std::string &message)
{
    for (const std::string &line : split_lines(output)) {
        if (!starts_with(line, "curl: (")) continue;
        const std::size_t close = line.find(')');
        if (close == std::string::npos) continue;
        if (!parse_int(line.substr(7, close - 7), code)) continue;
        message = trim(line.substr(close + 1));
        return true;
    }
    return false;
}

std::string version_text(const std::string &v)
{
    if (v.empty() || v == "0") return std::string();
    return "HTTP/" + v;
}

} // namespace

/* ---------------------------------------------------------------- the URL */

bool parse_http_target(const std::string &input, HttpTarget &out, std::string *why)
{
    out = HttpTarget();
    const auto fail = [&](const std::string &text) {
        if (why) *why = text;
        return false;
    };
    const std::string s = trim(input);
    if (s.empty()) return fail("Type an address: example.com or https://example.com/path");
    if (s.size() > 2048) return fail("The address is too long (2048 characters at most)");
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F) return fail("No spaces or control characters in an address");
        if (ch == '"' || ch == '\'' || ch == '`' || ch == ';' || ch == '$' || ch == '\\' || ch == '<' || ch == '>')
            return fail(std::string("This character is not allowed in an address: ") + ch);
    }
    if (s[0] == '-') return fail("An address cannot start with '-'");
    std::string rest = s;
    out.scheme = "https";
    const std::size_t sep = s.find("://");
    if (sep != std::string::npos) {
        const std::string scheme = lower(s.substr(0, sep));
        if (scheme != "http" && scheme != "https") return fail("Only http:// and https:// addresses can be checked");
        out.scheme = scheme;
        rest = s.substr(sep + 3);
    }
    const std::size_t end = rest.find_first_of("/?#");
    const std::string authority = rest.substr(0, end);
    const std::string path = end == std::string::npos ? std::string() : rest.substr(end);
    if (authority.empty()) return fail("The address has no host name");
    if (authority.find('@') != std::string::npos) return fail("No user name or password in the address");
    std::string host, port_text;
    bool v6 = false;
    if (authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return fail("A ']' is missing after the IPv6 address");
        host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return fail("Unexpected text after the IPv6 address");
            port_text = after.substr(1);
            if (port_text.empty()) return fail("The port is missing after ':'");
        }
        v6 = true;
    } else if (std::count(authority.begin(), authority.end(), ':') > 1) {
        host = authority; /* a bare IPv6 address, no port */
        v6 = true;
    } else {
        const std::size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            port_text = authority.substr(colon + 1);
            if (port_text.empty()) return fail("The port is missing after ':'");
        }
    }
    if (host.find('%') != std::string::npos) return fail("An address with an interface (%) cannot be checked");
    Address a;
    if (v6) {
        if (!parse_ip(host, a) || a.family != AF_INET6) return fail("Not a valid IPv6 address: " + host);
    } else {
        std::string hw;
        if (!valid_host(host, &hw)) return fail(hw);
    }
    if (!port_text.empty()) {
        int p = 0;
        for (const char c : port_text)
            if (c < '0' || c > '9') return fail("The port must be a number from 1 to 65535");
        if (port_text.size() > 5 || !parse_int(port_text, p) || p < 1 || p > 65535)
            return fail("The port must be a number from 1 to 65535");
        out.port = static_cast<std::uint16_t>(p);
        out.explicit_port = true;
    } else {
        out.port = out.scheme == "https" ? 443 : 80;
    }
    out.host = v6 ? a.text() : (host.size() > 1 && host.back() == '.' ? host.substr(0, host.size() - 1) : host);
    out.url = out.scheme + "://" + (v6 ? "[" + out.host + "]" : host) +
              (out.explicit_port ? ":" + std::to_string(out.port) : std::string()) + path;
    return true;
}

std::vector<std::string> curl_check_argv(const HttpTarget &t, const HttpCheckRequest &req)
{
    const int timeout = std::min(60, std::max(1, req.timeout_s));
    const int connect = std::min(30, std::max(1, req.connect_timeout_s));
    const int redirs = std::min(20, std::max(0, req.max_redirects));
    std::vector<std::string> argv = {"curl", "-sS", "-v", "-o", "/dev/null", "-g", "--proto", "=http,https",
                                     "--proto-redir", "=http,https", "-m", std::to_string(timeout),
                                     "--connect-timeout", std::to_string(connect), "--max-redirs",
                                     std::to_string(redirs)};
    if (req.follow) argv.push_back("-L");
    if (req.head) argv.push_back("-I");
    if (req.insecure) argv.push_back("-k");
    argv.push_back("-w");
    argv.push_back("%{json}\\n");
    argv.push_back("--url");
    argv.push_back(t.url);
    return argv;
}

std::vector<std::string> openssl_cert_argv(const HttpTarget &t)
{
    Address a;
    const bool literal = parse_ip(t.host, a);
    const std::string host = literal && a.family == AF_INET6 ? "[" + t.host + "]" : t.host;
    std::vector<std::string> argv = {"openssl", "s_client", "-connect", host + ":" + std::to_string(t.port)};
    if (!literal) { /* SNI is a name, never an address */
        argv.push_back("-servername");
        argv.push_back(t.host);
    }
    return argv;
}

/* ---------------------------------------------------------------- words */

const char *http_problem_name(HttpProblem p)
{
    switch (p) {
    case HttpProblem::None: return "ok";
    case HttpProblem::BadUrl: return "bad address";
    case HttpProblem::NoCurl: return "curl missing";
    case HttpProblem::Resolve: return "name not found";
    case HttpProblem::Refused: return "connection refused";
    case HttpProblem::Unreachable: return "unreachable";
    case HttpProblem::ConnectFailed: return "cannot connect";
    case HttpProblem::Timeout: return "timed out";
    case HttpProblem::CertExpired: return "certificate expired";
    case HttpProblem::CertNotYetValid: return "certificate not yet valid";
    case HttpProblem::CertNameMismatch: return "certificate name mismatch";
    case HttpProblem::CertSelfSigned: return "self-signed certificate";
    case HttpProblem::CertUntrusted: return "untrusted certificate";
    case HttpProblem::CertOther: return "certificate refused";
    case HttpProblem::TlsFailed: return "TLS failed";
    case HttpProblem::TooManyRedirects: return "too many redirects";
    case HttpProblem::EmptyReply: return "empty reply";
    case HttpProblem::Reset: return "connection reset";
    case HttpProblem::Protocol: return "not HTTP";
    case HttpProblem::Cancelled: return "cancelled";
    case HttpProblem::Other: return "failed";
    }
    return "failed";
}

bool is_cert_problem(HttpProblem p)
{
    return p == HttpProblem::CertExpired || p == HttpProblem::CertNotYetValid || p == HttpProblem::CertNameMismatch ||
           p == HttpProblem::CertSelfSigned || p == HttpProblem::CertUntrusted || p == HttpProblem::CertOther;
}

std::string x509_words(int code)
{
    switch (code) {
    case 0: return "ok";
    case 2: return "the issuer's certificate is unknown";
    case 9: return "certificate is not yet valid";
    case 10: return "certificate has expired";
    case 18: return "self-signed certificate";
    case 19: return "self-signed certificate in the chain";
    case 20: return "the issuer is not trusted (unknown authority)";
    case 21: return "the chain is incomplete (cannot verify the first certificate)";
    case 23: return "certificate revoked";
    case 26: return "certificate not meant for a server";
    case 62: return "the name does not match";
    default: return "verification error " + std::to_string(code);
    }
}

std::string http_reason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 511: return "Network Authentication Required";
    default: break;
    }
    if (status >= 100 && status < 200) return "Informational";
    if (status >= 200 && status < 300) return "Success";
    if (status >= 300 && status < 400) return "Redirect";
    if (status >= 400 && status < 500) return "Client error";
    if (status >= 500 && status < 600) return "Server error";
    return std::string();
}

/* ---------------------------------------------------------------- the certificate */

bool parse_cert_date(const std::string &text, std::int64_t &epoch)
{
    static const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::vector<std::string> f = split_ws(text);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (f.size() >= 4 && f[0].size() == 3) { /* "Sep 26 22:49:11 2026 GMT" */
        for (int i = 0; i < 12; ++i)
            if (lower(f[0]) == months[i]) mo = i + 1;
        if (!mo || !parse_int(f[1], d) || !parse_hms(f[2], h, mi, se) || !parse_int(f[3], y)) return false;
        if (f.size() >= 5 && f[4] != "GMT" && f[4] != "UTC" && f[4] != "Z") return false;
    } else if (f.size() >= 2 && f[0].size() == 10 && f[0][4] == '-' && f[0][7] == '-') { /* "2026-12-25 22:56:35 GMT" */
        if (!parse_int(f[0].substr(0, 4), y) || !parse_int(f[0].substr(5, 2), mo) || !parse_int(f[0].substr(8, 2), d) ||
            !parse_hms(f[1], h, mi, se))
            return false;
    } else {
        return false;
    }
    if (y < 1950 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    epoch = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

void cert_apply_clock(CertInfo &c, std::time_t now)
{
    c.has_days = false;
    c.expired = c.not_yet_valid = false;
    if (c.not_after_epoch) {
        const double diff = static_cast<double>(c.not_after_epoch - static_cast<std::int64_t>(now));
        c.days_left = static_cast<int>(std::floor(diff / 86400.0));
        c.has_days = true;
        c.expired = static_cast<std::int64_t>(now) > c.not_after_epoch;
    }
    if (c.not_before_epoch) c.not_yet_valid = static_cast<std::int64_t>(now) < c.not_before_epoch;
}

bool parse_curl_certs(const std::string &certs, CertInfo &out)
{
    out = CertInfo();
    bool in_first = false;
    for (const std::string &line : split_lines(certs)) {
        if (starts_with(line, "Subject:")) {
            if (in_first) break; /* the next certificate */
            in_first = true;
            out.subject = after_colon(line, 8);
        } else if (!in_first) {
            continue;
        } else if (starts_with(line, "Issuer:")) {
            out.issuer = after_colon(line, 7);
        } else if (starts_with(line, "Start date:")) {
            out.not_before = after_colon(line, 11);
        } else if (starts_with(line, "Expire date:")) {
            out.not_after = after_colon(line, 12);
        }
    }
    if (!in_first) return false;
    std::int64_t e = 0;
    if (parse_cert_date(out.not_before, e)) out.not_before_epoch = e;
    if (parse_cert_date(out.not_after, e)) out.not_after_epoch = e;
    out.present = true;
    out.source = "curl";
    return true;
}

bool parse_openssl_chain(const std::string &output, CertInfo &out, int *verify_code, std::string *verify_words)
{
    out = CertInfo();
    if (verify_code) *verify_code = -1;
    bool chain = false, first = false, done = false;
    std::string subject_eq, issuer_eq, before_eq, after_eq;
    for (const std::string &raw : split_lines(output)) {
        const std::string line = trim(raw);
        if (line == "Certificate chain") {
            chain = true;
            continue;
        }
        if (starts_with(line, "Verify return code:")) {
            const std::string rest = trim(line.substr(19));
            const std::size_t sp = rest.find(' ');
            int code = 0;
            if (parse_int(rest.substr(0, sp), code) && verify_code) *verify_code = code;
            if (verify_words && sp != std::string::npos) {
                std::string w = trim(rest.substr(sp));
                if (w.size() >= 2 && w.front() == '(' && w.back() == ')') w = w.substr(1, w.size() - 2);
                *verify_words = w;
            }
            continue;
        }
        /* the `openssl x509 -subject -issuer -dates` style, and s_client's "subject=" after the PEM */
        if (starts_with(line, "subject=") && subject_eq.empty()) subject_eq = trim(line.substr(8));
        else if (starts_with(line, "issuer=") && issuer_eq.empty()) issuer_eq = trim(line.substr(7));
        else if (starts_with(line, "notBefore=") && before_eq.empty()) before_eq = trim(line.substr(10));
        else if (starts_with(line, "notAfter=") && after_eq.empty()) after_eq = trim(line.substr(9));
        if (!chain || done) continue;
        if (line == "---") {
            if (first) done = true;
            continue;
        }
        if (starts_with(line, "0 s:")) {
            first = true;
            out.subject = trim(line.substr(4));
        } else if (first && (line.size() > 2 && line[0] >= '1' && line[0] <= '9' && line.find(" s:") != std::string::npos)) {
            done = true; /* the next certificate of the chain */
        } else if (first && starts_with(line, "i:")) {
            out.issuer = trim(line.substr(2));
        } else if (first && starts_with(line, "v:")) {
            /* "v:NotBefore: Sep 26 22:49:11 2026 GMT; NotAfter: Dec 25 22:56:35 2026 GMT" */
            const std::size_t nb = line.find("NotBefore:"), na = line.find("NotAfter:");
            if (nb != std::string::npos) {
                const std::size_t semi = line.find(';', nb);
                out.not_before = trim(line.substr(nb + 10, semi == std::string::npos ? std::string::npos : semi - nb - 10));
            }
            if (na != std::string::npos) out.not_after = trim(line.substr(na + 9));
        }
    }
    if (out.subject.empty()) out.subject = subject_eq;
    if (out.issuer.empty()) out.issuer = issuer_eq;
    if (out.not_before.empty()) out.not_before = before_eq;
    if (out.not_after.empty()) out.not_after = after_eq;
    if (out.subject.empty() && out.not_after.empty()) return false;
    std::int64_t e = 0;
    if (parse_cert_date(out.not_before, e)) out.not_before_epoch = e;
    if (parse_cert_date(out.not_after, e)) out.not_after_epoch = e;
    out.present = true;
    out.source = "openssl";
    return true;
}

/* ---------------------------------------------------------------- the problem */

HttpProblem classify_curl(int exit_code, const std::string &message, int ssl_verify_result, const std::string &output)
{
    const std::string m = lower(message);
    const auto cert = [&]() {
        if (contains(m, "has expired")) return HttpProblem::CertExpired;
        if (contains(m, "not yet valid")) return HttpProblem::CertNotYetValid;
        if (contains(m, "no alternative certificate subject name") || contains(m, "does not match target host") ||
            contains(m, "subject name does not match") || contains(m, "hostname mismatch"))
            return HttpProblem::CertNameMismatch;
        if (contains(m, "self-signed certificate in certificate chain") ||
            contains(m, "self signed certificate in certificate chain"))
            return HttpProblem::CertUntrusted;
        if (contains(m, "self-signed certificate") || contains(m, "self signed certificate"))
            return HttpProblem::CertSelfSigned;
        if (contains(m, "unable to get local issuer") || contains(m, "unable to verify the first certificate") ||
            contains(m, "unable to get issuer"))
            return HttpProblem::CertUntrusted;
        switch (ssl_verify_result) {
        case 9: return HttpProblem::CertNotYetValid;
        case 10: return HttpProblem::CertExpired;
        case 18: return HttpProblem::CertSelfSigned;
        case 2:
        case 19:
        case 20:
        case 21: return HttpProblem::CertUntrusted;
        case 62: return HttpProblem::CertNameMismatch;
        default: return HttpProblem::CertOther;
        }
    };
    switch (exit_code) {
    case 0: return HttpProblem::None;
    case 1:
    case 3: return HttpProblem::BadUrl;
    case 6: return HttpProblem::Resolve;
    case 7: {
        const std::string o = lower(output);
        if (contains(o, "connection refused")) return HttpProblem::Refused;
        if (contains(o, "no route to host") || contains(o, "network is unreachable") ||
            contains(o, "host is unreachable"))
            return HttpProblem::Unreachable;
        return HttpProblem::ConnectFailed;
    }
    case 28: return HttpProblem::Timeout;
    case 35: return contains(m, "certificate") ? cert() : HttpProblem::TlsFailed;
    case 47: return HttpProblem::TooManyRedirects;
    case 51:
    case 60: return cert();
    case 52: return HttpProblem::EmptyReply;
    case 55:
    case 56: return HttpProblem::Reset;
    case 8:
    case 16:
    case 92: return HttpProblem::Protocol;
    case 127: return HttpProblem::NoCurl;
    default: return HttpProblem::Other;
    }
}

std::string http_problem_words(HttpProblem p, const std::string &host, const std::string &message)
{
    switch (p) {
    case HttpProblem::None: return std::string();
    case HttpProblem::BadUrl: return "The address cannot be used" + (message.empty() ? "" : ": " + message);
    case HttpProblem::NoCurl: return "curl is not installed (package curl): the HTTP check needs it.";
    case HttpProblem::Resolve: return "The name " + host + " cannot be found (DNS). Check the spelling or the DNS server.";
    case HttpProblem::Refused: return "The server " + host + " refused the connection: nothing listens on that port.";
    case HttpProblem::Unreachable: return "No route to " + host + ": the network cannot reach it.";
    case HttpProblem::ConnectFailed: return "Could not connect to " + host + ".";
    case HttpProblem::Timeout:
        return contains(lower(message), "connection timed out")
                   ? "The server did not answer the connection in time (a firewall, or the host is off)."
                   : "The server did not finish answering in time.";
    case HttpProblem::CertExpired: return "The server's certificate has expired.";
    case HttpProblem::CertNotYetValid: return "The server's certificate is not valid yet (or the board's clock is wrong).";
    case HttpProblem::CertNameMismatch: return "The certificate is not made out to " + host + " (name mismatch).";
    case HttpProblem::CertSelfSigned: return "The certificate is self-signed: no known authority vouches for it.";
    case HttpProblem::CertUntrusted:
        return "The certificate's issuer is not trusted (an unknown authority or an incomplete chain).";
    case HttpProblem::CertOther: return "The certificate was refused" + (message.empty() ? "." : ": " + message);
    case HttpProblem::TlsFailed:
        return "The secure connection (TLS) could not be set up" + (message.empty() ? "." : ": " + message);
    case HttpProblem::TooManyRedirects: return "Too many redirects: the address may redirect in a loop.";
    case HttpProblem::EmptyReply: return "The server closed the connection without answering.";
    case HttpProblem::Reset: return "The connection was cut during the transfer.";
    case HttpProblem::Protocol: return "The server's answer is not valid HTTP.";
    case HttpProblem::Cancelled: return "Cancelled.";
    case HttpProblem::Other: return "curl failed" + (message.empty() ? "." : ": " + message);
    }
    return std::string();
}

HttpCheckResult parse_curl_check(const CommandResult &r, const HttpTarget &t, const HttpCheckRequest &req,
                                 std::time_t now)
{
    HttpCheckResult res;
    res.url = t.url;
    res.head = req.head;
    res.insecure = req.insecure;
    if (r.cancelled) {
        res.problem = HttpProblem::Cancelled;
        res.problem_words = http_problem_words(res.problem, t.host, "");
        return res;
    }
    if (!r.started) {
        res.problem = HttpProblem::Other;
        res.problem_words = "curl could not be started.";
        return res;
    }
    /* the report: a line that starts with {" (read from there on: a line cut in pieces still reads) */
    json::Object o;
    for (std::size_t at = 0; at < r.output.size();) {
        if (r.output.compare(at, 2, "{\"") == 0 && json::parse_object(r.output, at, o) &&
            (json::has(o, "http_code") || json::has(o, "exitcode"))) {
            res.reported = true;
            break;
        }
        o.clear();
        const std::size_t nl = r.output.find('\n', at);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    int exit_code = r.exit_code;
    std::string message;
    int line_code = 0;
    curl_error_line(r.output, line_code, message);
    if (res.reported) {
        res.status = static_cast<int>(json::get_number(o, "http_code", json::get_number(o, "response_code", 0)));
        res.final_url = json::get_string(o, "url_effective");
        res.redirect_url = json::get_string(o, "redirect_url");
        res.redirects = static_cast<int>(json::get_number(o, "num_redirects", 0));
        res.http_version = version_text(json::get_string(o, "http_version"));
        res.remote_ip = json::get_string(o, "remote_ip");
        res.remote_port = std::max(0, static_cast<int>(json::get_number(o, "remote_port", 0)));
        const double size = json::get_number(o, "size_download", 0);
        res.size_bytes = size > 0 ? static_cast<std::uint64_t>(size) : 0;
        res.content_type = json::get_string(o, "content_type");
        res.ssl_verify_result = static_cast<int>(json::get_number(o, "ssl_verify_result", 0));
        const std::string msg = json::get_string(o, "errormsg");
        if (!msg.empty()) message = msg;
        if (json::has(o, "exitcode") && r.exit_code < 0) exit_code = static_cast<int>(json::get_number(o, "exitcode", -1));
        const double dns = json::get_number(o, "time_namelookup", 0), con = json::get_number(o, "time_connect", 0);
        const double app = json::get_number(o, "time_appconnect", 0), pre = json::get_number(o, "time_pretransfer", 0);
        const double start = json::get_number(o, "time_starttransfer", 0), total = json::get_number(o, "time_total", 0);
        HttpTimings &tm = res.timings;
        tm.valid = total > 0;
        tm.connected = con > 0;
        tm.tls = app > 0;
        tm.first_byte = start > 0;
        tm.dns_ms = dns * 1000.0;
        tm.connect_ms = con > 0 ? std::max(0.0, con - dns) * 1000.0 : 0;
        tm.tls_ms = app > 0 ? std::max(0.0, app - con) * 1000.0 : 0;
        tm.wait_ms = start > 0 ? std::max(0.0, start - pre) * 1000.0 : 0;
        tm.transfer_ms = start > 0 ? std::max(0.0, total - start) * 1000.0 : 0;
        tm.redirect_ms = json::get_number(o, "time_redirect", 0) * 1000.0;
        tm.total_ms = total * 1000.0;
        if (parse_curl_certs(json::get_string(o, "certs"), res.cert)) cert_apply_clock(res.cert, now);
    }
    if (r.timed_out) {
        res.curl_exit = -1;
        res.problem = HttpProblem::Timeout;
        res.problem_words = "curl did not end in time.";
        res.answered = res.status > 0;
        return res;
    }
    res.curl_exit = exit_code;
    res.curl_message = message;
    res.answered = res.status > 0;
    if (res.answered) res.reason = http_reason(res.status);
    if (exit_code == 127 && !res.reported) res.problem = HttpProblem::NoCurl;
    else res.problem = classify_curl(exit_code, message, res.ssl_verify_result, r.output);
    if (res.problem == HttpProblem::None && !res.answered) {
        res.problem = res.reported ? HttpProblem::Protocol : HttpProblem::Other;
        if (!res.reported) message = "its report could not be read";
    }
    res.problem_words = http_problem_words(res.problem, t.host, message);
    if (res.problem == HttpProblem::TooManyRedirects)
        res.problem_words = "Too many redirects (more than " + std::to_string(std::max(0, req.max_redirects)) +
                            "): the address may redirect in a loop.";
    if (req.insecure && res.ssl_verify_result != 0 && t.scheme == "https")
        res.cert_note = "Certificate problem ignored: " + x509_words(res.ssl_verify_result);
    return res;
}

/* ---------------------------------------------------------------- the steps */

HttpCheckFlow::HttpCheckFlow(const HttpCheckRequest &req, WallClock clock) : req_(req), clock_(std::move(clock))
{
    std::string why;
    valid_ = parse_http_target(req.url, target_, &why);
    result_.url = valid_ ? target_.url : trim(req.url);
    result_.head = req.head;
    result_.insecure = req.insecure;
    if (!valid_) {
        result_.problem = HttpProblem::BadUrl;
        result_.problem_words = why;
        step_ = Step::Done;
    }
}

std::time_t HttpCheckFlow::now() const
{
    return clock_ ? clock_() : std::time(nullptr);
}

bool HttpCheckFlow::next(CommandSpec &cmd)
{
    cmd = CommandSpec();
    if (step_ == Step::Curl) {
        cmd.argv = curl_check_argv(target_, req_);
        cmd.timeout_ms = (std::min(60, std::max(1, req_.timeout_s)) + 5) * 1000;
        return true;
    }
    if (step_ == Step::Cert) {
        cmd.argv = openssl_cert_argv(target_);
        cmd.timeout_ms = 12000;
        cmd.max_output = 64 * 1024;
        return true;
    }
    return false;
}

void HttpCheckFlow::feed(const CommandResult &r)
{
    if (step_ == Step::Curl) {
        result_ = parse_curl_check(r, target_, req_, now());
        const bool want_cert = target_.scheme == "https" && !result_.cert.present && is_cert_problem(result_.problem);
        step_ = want_cert ? Step::Cert : Step::Done;
        return;
    }
    if (step_ == Step::Cert) {
        CertInfo c;
        if (!r.cancelled && r.started && parse_openssl_chain(r.output, c)) {
            cert_apply_clock(c, now());
            result_.cert = c;
        }
        step_ = Step::Done;
    }
}

/* ---------------------------------------------------------------- the session */

HttpCheckSession::HttpCheckSession() : runner_(std::make_shared<DirectRunner>()) {}

HttpCheckSession::HttpCheckSession(std::shared_ptr<CommandRunner> runner, WallClock clock)
    : runner_(std::move(runner)), clock_(std::move(clock))
{
}

bool HttpCheckSession::start(const HttpCheckRequest &req)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    HttpCheckSnapshot first;
    first.input = trim(req.url);
    auto flow = std::make_shared<HttpCheckFlow>(req, clock_);
    first.result = flow->result();
    if (!flow->valid()) {
        first.state = HttpCheckSnapshot::State::Failed;
        first.error = flow->result().problem_words;
        channel_.begin(gen, first);
        return false;
    }
    first.state = HttpCheckSnapshot::State::Running;
    first.step = "request";
    channel_.begin(gen, first);
    const std::shared_ptr<CommandRunner> runner = runner_;
    const std::uint64_t started = worker_.start([this, gen, first, flow, runner](const CancelToken &cancel) {
        HttpCheckSnapshot snap = first;
        const bool finished = run_flow(*flow, *runner, cancel, [&] {
            snap.result = flow->result();
            if (flow->step() == HttpCheckFlow::Step::Cert) {
                snap.step = "certificate";
                channel_.publish(gen, snap);
            }
        });
        snap.result = flow->result();
        snap.step.clear();
        snap.state = finished ? HttpCheckSnapshot::State::Done : HttpCheckSnapshot::State::Cancelled;
        channel_.publish(gen, snap);
    });
    if (!started) {
        first.state = HttpCheckSnapshot::State::Failed;
        first.error = "Could not start the check (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void HttpCheckSession::stop()
{
    worker_.cancel();
}

} // namespace nt
