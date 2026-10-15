/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the HTTP check (no LVGL; tests/test_httpcheck.cpp on curl reports recorded on the deck).
 *
 * One request with the system's curl (8.14.1 on the boards), never through a shell:
 *   curl -sS -v -o /dev/null -g --proto =http,https --proto-redir =http,https -m 15 --connect-timeout 8
 *        --max-redirs 5 [-L] [-I] [-k] -w '%{json}\n' --url <url>
 * `-w '%{json}'` prints one line of JSON with the status, the final URL, the redirect count, the HTTP version, the
 * remote address, the size, the timings and, for a verified TLS connection, the certificate chain ("certs":
 * "Subject:...\nIssuer:...\nStart date:...\nExpire date:..." per certificate, the server's first). `-v` is there only
 * for the reason of a failed connection: curl 8.14 says "Could not connect to server" for every connect failure, the
 * verbose line "connect to ... failed: Connection refused" tells a refusal from "No route to host".
 *
 * When curl refuses the certificate (exit 60: expired, self-signed, untrusted issuer, not yet valid) its report has no
 * certificate. Then a second command reads it without judging it:
 *   openssl s_client -connect <host>:<port> -servername <host>       (stdin /dev/null: it ends after the handshake)
 * whose "Certificate chain" section gives " 0 s:<subject>", "   i:<issuer>" and "   v:NotBefore: ...; NotAfter: ...".
 * Checked on the deck (curl 8.14.1, OpenSSL 3.5.7) on 2026-10-15; see README "HTTP check".
 *
 * The days left come from an injectable wall clock. Timings are stage durations in ms: dns = namelookup,
 * connect = connect - namelookup, tls = appconnect - connect, wait = starttransfer - pretransfer,
 * transfer = total - starttransfer (curl adds the requests of a redirect chain together; redirect_ms is their part).
 */
#pragma once

#include "cmdrun.hpp"
#include "worker.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

using WallClock = std::function<std::time_t()>; /* nullptr = time(nullptr) */

/* ---------------------------------------------------------------- the URL */

struct HttpTarget {
    std::string url;             /* what curl gets: "https://example.com", "http://[2001:db8::1]:8080/x" */
    std::string scheme;          /* "https" or "http" */
    std::string host;            /* a name or an address (IPv6 without brackets) */
    std::uint16_t port = 0;      /* the one typed, else 443 / 80 */
    bool explicit_port = false;
};

/* host, host:port, http://..., https://... (https when no scheme). Refused: empty, longer than 2048, blanks or control
 * bytes, quotes, ';', '`', '$', '\\', '<', '>', a leading '-', another scheme, a bad host or port, user:password@. */
bool parse_http_target(const std::string &input, HttpTarget &out, std::string *why = nullptr);

struct HttpCheckRequest {
    std::string url;             /* as typed */
    bool head = false;           /* -I instead of GET */
    bool follow = true;          /* -L */
    bool insecure = false;       /* -k: only when the user asked to ignore certificate errors */
    int timeout_s = 15;          /* -m (1 .. 60) */
    int connect_timeout_s = 8;   /* --connect-timeout (1 .. 30) */
    int max_redirects = 5;       /* --max-redirs (0 .. 20) */
};

std::vector<std::string> curl_check_argv(const HttpTarget &t, const HttpCheckRequest &req);
std::vector<std::string> openssl_cert_argv(const HttpTarget &t);

/* ---------------------------------------------------------------- the results */

enum class HttpProblem {
    None,              /* an HTTP answer came (whatever its status) */
    BadUrl,
    NoCurl,            /* curl is not installed */
    Resolve,           /* the name does not resolve */
    Refused,           /* the server refused the connection */
    Unreachable,       /* no route */
    ConnectFailed,     /* could not connect, no detail */
    Timeout,
    CertExpired,
    CertNotYetValid,
    CertNameMismatch,
    CertSelfSigned,
    CertUntrusted,     /* issuer not trusted, incomplete chain, self-signed root */
    CertOther,
    TlsFailed,         /* the TLS handshake failed */
    TooManyRedirects,
    EmptyReply,
    Reset,             /* the connection broke */
    Protocol,          /* the server's answer was not valid HTTP */
    Cancelled,
    Other
};

/* "certificate expired", ... (short, for a badge) */
const char *http_problem_name(HttpProblem p);
bool is_cert_problem(HttpProblem p);

struct CertInfo {
    bool present = false;
    std::string source;          /* "curl" or "openssl" */
    std::string subject;         /* as printed: "CN = example.com" / "CN=example.com" */
    std::string issuer;
    std::string not_before;      /* as printed: "Sep 26 22:49:11 2026 GMT" */
    std::string not_after;
    std::int64_t not_before_epoch = 0; /* 0 when unreadable */
    std::int64_t not_after_epoch = 0;
    bool has_days = false;
    int days_left = 0;           /* floor((not_after - now) / 1 day): negative once expired */
    bool expired = false;
    bool not_yet_valid = false;
};

struct HttpTimings {
    bool valid = false;
    bool connected = false;      /* a TCP connection was made (connect_ms means something) */
    bool tls = false;            /* a TLS handshake happened */
    bool first_byte = false;     /* the answer started (wait_ms and transfer_ms mean something) */
    double dns_ms = 0, connect_ms = 0, tls_ms = 0, wait_ms = 0, transfer_ms = 0, redirect_ms = 0, total_ms = 0;
};

struct HttpCheckResult {
    std::string url;             /* the one asked */
    bool head = false, insecure = false;
    bool reported = false;       /* curl's JSON report was read */
    bool answered = false;       /* an HTTP status came */
    int status = 0;
    std::string reason;          /* "OK", "Not Found", ... */
    std::string final_url;
    std::string redirect_url;    /* a redirect not followed */
    int redirects = 0;
    std::string http_version;    /* "HTTP/2", "HTTP/1.1", "HTTP/3" */
    std::string remote_ip;
    int remote_port = 0;
    std::uint64_t size_bytes = 0;
    std::string content_type;
    HttpTimings timings;
    CertInfo cert;
    int ssl_verify_result = 0;   /* OpenSSL's X509 code: 0 = ok */
    std::string cert_note;       /* -k with a certificate that does not verify: what is wrong with it */
    int curl_exit = -1;
    std::string curl_message;    /* curl's own error message */
    HttpProblem problem = HttpProblem::None;
    std::string problem_words;   /* plain words */
};

/* "OK", "Moved Permanently", ...; "Success" / "Redirect" / "Client error" / "Server error" for the others */
std::string http_reason(int status);

/* "Sep 26 22:49:11 2026 GMT", "Apr  9 00:00:00 2015 GMT" (also "2026-12-25 22:56:35 GMT") -> UTC seconds */
bool parse_cert_date(const std::string &text, std::int64_t &epoch);
/* days left and the expired / not-yet-valid flags for that time */
void cert_apply_clock(CertInfo &c, std::time_t now);

/* curl's "certs" text: the first certificate (the server's) */
bool parse_curl_certs(const std::string &certs, CertInfo &out);
/* openssl s_client's output: the chain's first certificate, and "Verify return code: N (words)" */
bool parse_openssl_chain(const std::string &output, CertInfo &out, int *verify_code = nullptr,
                         std::string *verify_words = nullptr);

/* curl's exit code, its message and the verbose connect lines -> the problem */
HttpProblem classify_curl(int exit_code, const std::string &message, int ssl_verify_result,
                          const std::string &output = "");
/* The words for a problem (host and the curl message give detail). */
std::string http_problem_words(HttpProblem p, const std::string &host, const std::string &message);
/* OpenSSL's X509 verify codes in words: 10 -> "certificate has expired", ... */
std::string x509_words(int code);

/* The whole curl report (the command's result) -> the result. now: for the days left. */
HttpCheckResult parse_curl_check(const CommandResult &r, const HttpTarget &t, const HttpCheckRequest &req,
                                 std::time_t now);

/* ---------------------------------------------------------------- the steps */

/* curl, then (only when curl refused the certificate) openssl for the certificate. */
class HttpCheckFlow : public CommandFlow {
public:
    explicit HttpCheckFlow(const HttpCheckRequest &req, WallClock clock = nullptr);
    bool valid() const { return valid_; }       /* false: a bad URL (result().problem_words says why) */
    bool next(CommandSpec &cmd) override;
    void feed(const CommandResult &r) override;
    const HttpCheckResult &result() const { return result_; }
    const HttpTarget &target() const { return target_; }
    enum class Step { Curl, Cert, Done };
    Step step() const { return step_; }

private:
    std::time_t now() const;
    HttpCheckRequest req_;
    WallClock clock_;
    HttpTarget target_;
    HttpCheckResult result_;
    bool valid_ = false;
    Step step_ = Step::Curl;
};

/* ---------------------------------------------------------------- the session */

struct HttpCheckSnapshot {
    enum class State { Idle, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string input;           /* as typed */
    std::string step;            /* Running: "request" or "certificate" */
    std::string error;           /* Failed: why nothing ran (a bad URL) */
    HttpCheckResult result;
    bool running() const { return state == State::Running; }
};

class HttpCheckSession {
public:
    HttpCheckSession();          /* DirectRunner, the system clock */
    HttpCheckSession(std::shared_ptr<CommandRunner> runner, WallClock clock);
    bool start(const HttpCheckRequest &req); /* LVGL thread; false (a Failed snapshot) for a bad URL */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const HttpCheckSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<CommandRunner> runner_;
    WallClock clock_;
    Channel<HttpCheckSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
