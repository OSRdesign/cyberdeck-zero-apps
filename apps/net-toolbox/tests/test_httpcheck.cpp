/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the HTTP check. The JSON reader; the URL rules with hostile inputs; the command lines; curl's
 * reports recorded on the deck (tests/fixtures/curl, curl 8.14.1 / OpenSSL 3.5.7, 2026-10-15: a healthy HTTPS page,
 * HEAD, a redirect followed and not followed, too many redirects, an expired, a self-signed, an untrusted-root and a
 * wrong-name certificate, -k on an expired one, a DNS failure, a refused connection (with -v), a connect timeout, a TLS
 * reset) and openssl s_client's (tests/fixtures/openssl); the days left on an injected clock (expiring soon, expired);
 * the steps (curl, then openssl only for a refused certificate) on a fake runner and on fake curl / openssl programs
 * run by the real DirectRunner; the session and its cancel.
 */
#include "check.hpp"

#include "httpcheck.hpp"
#include "minijson.hpp"
#include "text.hpp"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

using namespace nt;

namespace {

std::time_t at(const char *date)
{
    std::int64_t e = 0;
    parse_cert_date(date, e);
    return static_cast<std::time_t>(e);
}

const std::time_t kDeckDay = at("Oct 15 12:00:00 2026 GMT"); /* when the fixtures were recorded */

CommandResult exited(int code, const std::string &output)
{
    CommandResult r;
    r.started = true;
    r.exit_code = code;
    r.output = output;
    return r;
}

CommandResult curl_fixture(const char *name, int code)
{
    return exited(code, fixture("curl", (std::string(name) + ".txt").c_str()));
}

HttpCheckResult check(const char *url, const CommandResult &r, std::time_t now = kDeckDay, bool insecure = false,
                      bool follow = true)
{
    HttpCheckRequest req;
    req.url = url;
    req.insecure = insecure;
    req.follow = follow;
    HttpTarget t;
    parse_http_target(url, t);
    return parse_curl_check(r, t, req, now);
}

/* Answers by program name; records what it was asked. */
class FakeRunner : public CommandRunner {
public:
    std::map<std::string, CommandResult> answers;
    std::vector<std::vector<std::string>> asked;
    bool block_until_cancel = false;
    CommandResult run(const CommandSpec &spec, const CancelToken &cancel) override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            asked.push_back(spec.argv);
        }
        if (block_until_cancel) {
            while (!cancel.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            CommandResult r;
            r.started = true;
            r.cancelled = true;
            return r;
        }
        const auto it = answers.find(spec.argv.empty() ? std::string() : spec.argv[0]);
        if (it == answers.end()) return exited(127, "");
        return it->second;
    }

private:
    std::mutex mu_;
};

void test_minijson()
{
    json::Object o;
    std::size_t end = 0;
    const std::string text = "{\"a\":\"x\\ny\\u00e9\\ud83d\\ude00\\t\",\"n\":-1.5e2,\"t\":true,\"z\":null,"
                             "\"nest\":{\"k\":[1,2,{\"q\":\"]\"}]},\"e\":\"\"} trailing";
    CHECK(json::parse_object(text, 0, o, &end));
    CHECK_EQ(json::get_string(o, "a"), std::string("x\ny\xC3\xA9\xF0\x9F\x98\x80\t"));
    CHECK_NEAR(json::get_number(o, "n"), -150.0, 1e-9);
    CHECK(o["t"].kind == json::Value::Kind::Bool && o["z"].kind == json::Value::Kind::Null);
    CHECK(o["nest"].kind == json::Value::Kind::Nested && json::get_string(o, "e") == "" && json::has(o, "e"));
    CHECK_EQ(text.substr(end), std::string(" trailing"));
    CHECK_EQ(json::get_string(o, "missing", "def"), std::string("def"));
    CHECK_NEAR(json::get_number(o, "a", 7), 7.0, 1e-9); /* a string is not a number */
    CHECK(json::parse_object("{\"s\":\"\\ud800x\"}", 0, o) && json::get_string(o, "s") == "?x"); /* lone surrogate */
    CHECK(json::parse_object("{\"cut\":\"ab\ncd\"}", 0, o) && json::get_string(o, "cut") == "abcd");
    CHECK(json::parse_object("{}", 0, o) && o.empty());
    for (const char *bad : {"", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":tru}", "{\"a\":\"x}", "[1]",
                            "{\"a\":\"\\q\"}", "{\"a\":1 \"b\":2}", "{\"a\":\"\\u12\"}", "{\"a\":1e999}"})
        CHECK(!json::parse_object(bad, 0, o));
    std::string deep = "{\"d\":";
    for (int i = 0; i < 40; ++i) deep += "[";
    for (int i = 0; i < 40; ++i) deep += "]";
    deep += "}";
    CHECK(!json::parse_object(deep, 0, o)); /* nesting is bounded */
}

void test_http_url()
{
    HttpTarget t;
    std::string why;
    CHECK(parse_http_target("example.com", t) && t.url == "https://example.com" && t.scheme == "https" &&
          t.host == "example.com" && t.port == 443 && !t.explicit_port);
    CHECK(parse_http_target("  example.com:8080  ", t) && t.url == "https://example.com:8080" && t.port == 8080);
    CHECK(parse_http_target("http://neverssl.com/x?a=1&b=2#f", t) && t.url == "http://neverssl.com/x?a=1&b=2#f" &&
          t.port == 80 && t.scheme == "http");
    CHECK(parse_http_target("HTTPS://Example.COM/", t) && t.scheme == "https" && t.url == "https://Example.COM/");
    CHECK(parse_http_target("https://[2001:db8::1]:8443/x", t) && t.host == "2001:db8::1" && t.port == 8443 &&
          t.url == "https://[2001:db8::1]:8443/x");
    CHECK(parse_http_target("2606:4700::1111", t) && t.url == "https://[2606:4700::1111]" && t.port == 443);
    CHECK(parse_http_target("192.168.68.1", t) && t.url == "https://192.168.68.1" && t.host == "192.168.68.1");
    CHECK(parse_http_target("example.com.", t) && t.host == "example.com");
    const char *hostile[] = {"",
                             "   ",
                             "exa mple.com",
                             "example.com/a b",
                             "example.com\n-o /etc/passwd",
                             "example.com\r\n/x",
                             "'example.com'",
                             "\"example.com\"",
                             "`reboot`.com",
                             "example.com;reboot",
                             "$(reboot)",
                             "example.com/$HOME",
                             "-o/tmp/x",
                             "--config=/etc/passwd",
                             "-K/etc/passwd",
                             "file:///etc/passwd",
                             "ftp://example.com",
                             "gopher://example.com",
                             "dict://example.com",
                             "telnet://example.com:23",
                             "https://user:pass@example.com",
                             "https://example.com:0",
                             "https://example.com:65536",
                             "https://example.com:",
                             "https://example.com:8o",
                             "https://",
                             "https:///path",
                             "http://exa\\mple.com",
                             "https://<script>",
                             "https://[2001:db8::1",
                             "https://[2001:db8::1]x",
                             "https://[nothex::1]",
                             "https://fe80::1%wlan0",
                             "https://bad_label-.-x..com",
                             "example.com\x01"};
    for (const char *h : hostile) {
        why.clear();
        const bool ok = parse_http_target(h, t, &why);
        CHECK(!ok && !why.empty());
        if (ok) std::fprintf(stderr, "  accepted: [%s]\n", test_text(h).c_str());
    }
    CHECK(!parse_http_target(std::string(2049, 'a'), t, &why) && why.find("too long") != std::string::npos);
    CHECK(parse_http_target("example.com\r\n", t) && t.url == "https://example.com"); /* blanks around: trimmed */

    // the command lines: an argv, never a shell; -k only when asked
    HttpCheckRequest req;
    parse_http_target("example.com", t);
    std::vector<std::string> a = curl_check_argv(t, req);
    const std::vector<std::string> want = {"curl", "-sS", "-v", "-o", "/dev/null", "-g", "--proto", "=http,https",
                                           "--proto-redir", "=http,https", "-m", "15", "--connect-timeout", "8",
                                           "--max-redirs", "5", "-L", "-w", "%{json}\\n", "--url",
                                           "https://example.com"};
    CHECK(a == want);
    req.head = true;
    req.insecure = true;
    req.follow = false;
    req.timeout_s = 999;
    req.connect_timeout_s = 0;
    req.max_redirects = -3;
    a = curl_check_argv(t, req);
    CHECK(std::find(a.begin(), a.end(), "-I") != a.end() && std::find(a.begin(), a.end(), "-k") != a.end());
    CHECK(std::find(a.begin(), a.end(), "-L") == a.end());
    CHECK(a[11] == "60" && a[13] == "1" && a[15] == "0"); /* clamped */
    CHECK(openssl_cert_argv(t) == std::vector<std::string>({"openssl", "s_client", "-connect", "example.com:443",
                                                            "-servername", "example.com"}));
    parse_http_target("https://[2001:db8::1]:8443/", t);
    CHECK(openssl_cert_argv(t) == std::vector<std::string>({"openssl", "s_client", "-connect", "[2001:db8::1]:8443"}));
    parse_http_target("10.0.0.1", t);
    CHECK(openssl_cert_argv(t).size() == 4); /* no SNI for an address */
}

void test_httpcheck_parse()
{
    // dates
    std::int64_t e = 0;
    CHECK(parse_cert_date("Jan  1 00:00:00 1970 GMT", e) && e == 0);
    CHECK(parse_cert_date("Apr  9 00:00:00 2015 GMT", e) && e == 1428537600);
    CHECK(parse_cert_date("2015-04-09 00:00:00 GMT", e) && e == 1428537600);
    CHECK(parse_cert_date("Feb 29 12:00:00 2028 GMT", e));
    for (const char *bad : {"", "Foo 1 00:00:00 2015 GMT", "Sep 26 25:00:00 2026 GMT", "Sep 26 22:49:11 2026 CET",
                            "Sep 26 2026", "Sep 26 22:49 2026 GMT", "Sep 40 22:49:11 2026 GMT"})
        CHECK(!parse_cert_date(bad, e));

    // a healthy HTTPS page
    HttpCheckResult r = check("example.com", curl_fixture("https-ok", 0));
    CHECK(r.reported && r.answered && r.status == 200 && r.reason == "OK" && r.problem == HttpProblem::None);
    CHECK(r.problem_words.empty() && r.curl_exit == 0 && r.curl_message.empty());
    CHECK_EQ(r.http_version, std::string("HTTP/2"));
    CHECK_EQ(r.final_url, std::string("https://example.com/"));
    CHECK(r.remote_ip == "172.66.147.243" && r.remote_port == 443 && r.size_bytes == 577 && r.redirects == 0);
    CHECK_EQ(r.content_type, std::string("text/html; charset=utf-8"));
    CHECK(r.timings.valid && r.timings.tls && r.timings.connected && r.timings.first_byte);
    CHECK_NEAR(r.timings.dns_ms, 39.637, 1e-6);
    CHECK_NEAR(r.timings.connect_ms, 15.433, 1e-6);
    CHECK_NEAR(r.timings.tls_ms, 143.871, 1e-6);
    CHECK_NEAR(r.timings.wait_ms, 81.361, 1e-6);
    CHECK_NEAR(r.timings.transfer_ms, 0.445, 1e-6);
    CHECK_NEAR(r.timings.total_ms, 281.357, 1e-6);
    CHECK(r.cert.present && r.cert.source == "curl");
    CHECK_EQ(r.cert.subject, std::string("CN = example.com"));
    CHECK_EQ(r.cert.issuer, std::string("C = US, O = SSL Corporation, CN = Cloudflare TLS Issuing ECC CA 3"));
    CHECK_EQ(r.cert.not_before, std::string("Sep 26 22:49:11 2026 GMT"));
    CHECK_EQ(r.cert.not_after, std::string("Dec 25 22:56:35 2026 GMT"));
    CHECK(r.cert.has_days && r.cert.days_left == 71 && !r.cert.expired && !r.cert.not_yet_valid);
    // the same certificate on other days: expiring soon, the last day, expired, not yet valid
    r = check("example.com", curl_fixture("https-ok", 0), at("Dec 20 12:00:00 2026 GMT"));
    CHECK(r.cert.days_left == 5 && !r.cert.expired);
    r = check("example.com", curl_fixture("https-ok", 0), at("Dec 25 22:56:34 2026 GMT"));
    CHECK(r.cert.days_left == 0 && !r.cert.expired);
    r = check("example.com", curl_fixture("https-ok", 0), at("Dec 26 12:00:00 2026 GMT"));
    CHECK(r.cert.days_left == -1 && r.cert.expired);
    r = check("example.com", curl_fixture("https-ok", 0), at("Sep 01 00:00:00 2026 GMT"));
    CHECK(r.cert.not_yet_valid);
    // HEAD
    r = check("example.com", curl_fixture("head", 0));
    CHECK(r.status == 200 && r.size_bytes == 0 && r.cert.present);
    // a redirect chain followed: http -> https
    r = check("http://github.com", curl_fixture("redirect-followed", 0));
    CHECK(r.status == 200 && r.redirects == 1 && r.final_url == "https://github.com/" && r.url == "http://github.com");
    CHECK_NEAR(r.timings.redirect_ms, 62.849, 1e-6);
    CHECK(r.cert.present && r.cert.subject == "CN = github.com");
    // not followed: the status and where it points
    r = check("http://github.com", curl_fixture("redirect-not-followed", 0), kDeckDay, false, false);
    CHECK(r.status == 301 && r.reason == "Moved Permanently" && r.redirect_url == "https://github.com/");
    CHECK(r.problem == HttpProblem::None && !r.timings.tls && r.timings.tls_ms == 0 && !r.cert.present);
    // too many redirects: an answer came, but the check failed
    r = check("http://github.com", curl_fixture("too-many-redirects", 47));
    CHECK(r.problem == HttpProblem::TooManyRedirects && r.answered && r.status == 301);
    CHECK(r.problem_words.find("Too many redirects (more than 5)") == 0);
    // certificates curl refused: no certificate in its report
    r = check("expired.badssl.com", curl_fixture("expired", 60));
    CHECK(r.problem == HttpProblem::CertExpired && !r.answered && !r.cert.present && r.ssl_verify_result == 10);
    CHECK(r.timings.connected && !r.timings.tls && !r.timings.first_byte); /* the handshake failed */
    CHECK(r.curl_message == "SSL certificate problem: certificate has expired" && is_cert_problem(r.problem));
    CHECK_EQ(r.problem_words, std::string("The server's certificate has expired."));
    r = check("self-signed.badssl.com", curl_fixture("self-signed", 60));
    CHECK(r.problem == HttpProblem::CertSelfSigned && r.ssl_verify_result == 18);
    r = check("untrusted-root.badssl.com", curl_fixture("untrusted-root", 60));
    CHECK(r.problem == HttpProblem::CertUntrusted && r.ssl_verify_result == 19);
    r = check("wrong.host.badssl.com", curl_fixture("name-mismatch", 60));
    CHECK(r.problem == HttpProblem::CertNameMismatch && r.cert.present && r.cert.subject == "CN = *.badssl.com");
    CHECK(r.problem_words.find("not made out to wrong.host.badssl.com") != std::string::npos);
    // -k on an expired certificate: the page comes, the note says what was ignored, the dates are there
    r = check("expired.badssl.com", curl_fixture("expired-insecure", 0), kDeckDay, true);
    CHECK(r.problem == HttpProblem::None && r.status == 200 && r.http_version == "HTTP/1.1" && r.insecure);
    CHECK(r.cert.present && r.cert.expired && r.cert.days_left < -4000);
    CHECK_EQ(r.cert_note, std::string("Certificate problem ignored: the issuer is not trusted (unknown authority)"));
    // the network
    r = check("https://nonexistent.invalid", curl_fixture("dns-fail", 6));
    CHECK(r.problem == HttpProblem::Resolve && r.problem_words.find("nonexistent.invalid cannot be found") != std::string::npos);
    CHECK(r.timings.valid && !r.timings.connected && !r.timings.first_byte);
    r = check("http://127.0.0.1:9", curl_fixture("refused-verbose", 7));
    CHECK(r.problem == HttpProblem::Refused && r.problem_words.find("refused the connection") != std::string::npos);
    r = check("http://10.255.255.1", curl_fixture("timeout", 28));
    CHECK(r.problem == HttpProblem::Timeout && r.problem_words.find("did not answer the connection") != std::string::npos);
    r = check("https://wrong.host.badssl.com", curl_fixture("tls-reset", 35));
    CHECK(r.problem == HttpProblem::TlsFailed && r.problem_words.find("Connection reset by peer") != std::string::npos);
    /* constructed (the deck has no unreachable network to record): curl 8.14's verbose line for no route */
    r = check("http://10.9.9.9", exited(7, "*   Trying 10.9.9.9:80...\n* connect to 10.9.9.9 port 80 from 0.0.0.0 port "
                                           "0 failed: No route to host\ncurl: (7) Failed to connect to 10.9.9.9 port 80 "
                                           "after 3 ms: Could not connect to server\n"));
    CHECK(r.problem == HttpProblem::Unreachable && !r.reported);
    r = check("http://10.9.9.9", exited(7, "curl: (7) Failed to connect\n"));
    CHECK(r.problem == HttpProblem::ConnectFailed);
    // the runner's own outcomes
    r = check("example.com", exited(127, ""));
    CHECK(r.problem == HttpProblem::NoCurl && r.problem_words.find("package curl") != std::string::npos);
    CommandResult late;
    late.started = true;
    late.timed_out = true;
    CHECK(check("example.com", late).problem == HttpProblem::Timeout);
    CommandResult stopped;
    stopped.cancelled = true;
    CHECK(check("example.com", stopped).problem == HttpProblem::Cancelled);
    r = check("example.com", exited(0, "<html>not a report</html>\n"));
    CHECK(r.problem == HttpProblem::Other && !r.reported && r.problem_words.find("report could not be read") != std::string::npos);
    r = check("example.com", exited(0, "{\"http_code\":200,\"certs\":\"\",\"time_total\":0.1}\n"));
    CHECK(r.problem == HttpProblem::None && r.status == 200 && !r.cert.present);
    // a report cut in pieces inside a string (async_run with a short max_line) still reads
    std::string cut = fixture("curl", "https-ok.txt");
    const std::size_t in_certs = cut.find("Subject:CN = example.com") + 10;
    cut.insert(in_certs, "\n");
    r = check("example.com", exited(0, cut));
    CHECK(r.status == 200 && r.cert.present && r.cert.days_left == 71);
    // classification by exit code and words
    CHECK(classify_curl(60, "", 10) == HttpProblem::CertExpired);
    CHECK(classify_curl(60, "", 9) == HttpProblem::CertNotYetValid);
    CHECK(classify_curl(60, "", 62) == HttpProblem::CertNameMismatch);
    CHECK(classify_curl(60, "SSL certificate problem: unable to get local issuer certificate", 20) ==
          HttpProblem::CertUntrusted);
    CHECK(classify_curl(60, "SSL certificate problem: certificate is not yet valid", 0) == HttpProblem::CertNotYetValid);
    CHECK(classify_curl(60, "something new", 23) == HttpProblem::CertOther);
    CHECK(classify_curl(52, "Empty reply from server", 0) == HttpProblem::EmptyReply);
    CHECK(classify_curl(56, "Recv failure", 0) == HttpProblem::Reset);
    CHECK(classify_curl(3, "URL rejected", 0) == HttpProblem::BadUrl);
    CHECK(classify_curl(92, "HTTP/2 stream", 0) == HttpProblem::Protocol);
    CHECK(classify_curl(99, "", 0) == HttpProblem::Other);
    CHECK_EQ(http_reason(404), std::string("Not Found"));
    CHECK_EQ(http_reason(299), std::string("Success"));
    CHECK_EQ(http_reason(599), std::string("Server error"));
    CHECK_EQ(http_reason(42), std::string(""));
    CHECK_EQ(x509_words(10), std::string("certificate has expired"));
    CHECK_EQ(x509_words(77), std::string("verification error 77"));
    CHECK_EQ(std::string(http_problem_name(HttpProblem::CertSelfSigned)), std::string("self-signed certificate"));

    // openssl: the chain's first certificate, the verify code
    CertInfo c;
    int code = -1;
    std::string words;
    CHECK(parse_openssl_chain(fixture("openssl", "s_client-expired.txt"), c, &code, &words));
    CHECK_EQ(c.subject, std::string("OU=Domain Control Validated, OU=PositiveSSL Wildcard, CN=*.badssl.com"));
    CHECK_EQ(c.issuer, std::string("C=GB, ST=Greater Manchester, L=Salford, O=COMODO CA Limited, CN=COMODO RSA Domain "
                                   "Validation Secure Server CA"));
    CHECK(c.not_before == "Apr  9 00:00:00 2015 GMT" && c.not_after == "Apr 12 23:59:59 2015 GMT");
    CHECK(code == 10 && words == "certificate has expired" && c.source == "openssl");
    cert_apply_clock(c, kDeckDay);
    CHECK(c.expired && c.days_left < -4000);
    CHECK(parse_openssl_chain(fixture("openssl", "s_client-ok.txt"), c, &code));
    CHECK(c.subject == "CN=example.com" && c.not_after == "Dec 25 22:56:35 2026 GMT" && code == 0);
    CHECK(parse_openssl_chain(fixture("openssl", "x509-dates.txt"), c, &code)); /* the `openssl x509 -dates` form */
    CHECK(c.subject == "CN=example.com" && c.not_before == "Sep 26 22:49:11 2026 GMT" && code == -1);
    CHECK(!parse_openssl_chain("connect: Connection refused\nconnect:errno=111\n", c));
    CHECK(!parse_curl_certs("", c) && !parse_curl_certs("garbage\nmore", c));
}

void test_httpcheck_flow()
{
    const WallClock clock = [] { return kDeckDay; };
    // healthy: one curl, no openssl
    {
        FakeRunner run;
        run.answers["curl"] = curl_fixture("https-ok", 0);
        HttpCheckRequest req;
        req.url = "example.com";
        HttpCheckFlow flow(req, clock);
        CHECK(flow.valid());
        CHECK(run_flow(flow, run, CancelToken()));
        CHECK(run.asked.size() == 1 && run.asked[0][0] == "curl" && run.asked[0].back() == "https://example.com");
        CHECK(flow.result().status == 200 && flow.result().cert.days_left == 71 && flow.step() == HttpCheckFlow::Step::Done);
    }
    // an expired certificate: curl, then openssl for the dates
    {
        FakeRunner run;
        run.answers["curl"] = curl_fixture("expired", 60);
        run.answers["openssl"] = exited(0, fixture("openssl", "s_client-expired.txt"));
        HttpCheckRequest req;
        req.url = "expired.badssl.com";
        HttpCheckFlow flow(req, clock);
        CHECK(run_flow(flow, run, CancelToken()));
        CHECK(run.asked.size() == 2);
        if (run.asked.size() == 2)
            CHECK(run.asked[1] == std::vector<std::string>({"openssl", "s_client", "-connect", "expired.badssl.com:443",
                                                            "-servername", "expired.badssl.com"}));
        const HttpCheckResult &r = flow.result();
        CHECK(r.problem == HttpProblem::CertExpired && r.cert.present && r.cert.source == "openssl" && r.cert.expired);
    }
    // openssl missing or failing: the curl result stays, without a certificate
    {
        FakeRunner run;
        run.answers["curl"] = curl_fixture("self-signed", 60);
        HttpCheckRequest req;
        req.url = "self-signed.badssl.com";
        HttpCheckFlow flow(req, clock);
        CHECK(run_flow(flow, run, CancelToken()));
        CHECK(run.asked.size() == 2 && flow.result().problem == HttpProblem::CertSelfSigned && !flow.result().cert.present);
    }
    // not a certificate problem, or plain http: no openssl
    {
        FakeRunner run;
        run.answers["curl"] = curl_fixture("dns-fail", 6);
        HttpCheckRequest req;
        req.url = "nonexistent.invalid";
        HttpCheckFlow flow(req, clock);
        run_flow(flow, run, CancelToken());
        CHECK(run.asked.size() == 1 && flow.result().problem == HttpProblem::Resolve);
    }
    // a bad address: nothing runs
    {
        HttpCheckRequest req;
        req.url = "file:///etc/passwd";
        HttpCheckFlow flow(req, clock);
        CommandSpec cmd;
        CHECK(!flow.valid() && !flow.next(cmd) && flow.result().problem == HttpProblem::BadUrl);
    }
    // the session: Done with the result; a bad URL fails at once; a stop ends it within a slice
    {
        auto run = std::make_shared<FakeRunner>();
        run->answers["curl"] = curl_fixture("https-ok", 0);
        HttpCheckSession s(run, clock);
        HttpCheckRequest req;
        req.url = "example.com";
        CHECK(s.start(req));
        CHECK(s.wait(2000));
        auto snap = s.latest();
        CHECK(snap->state == HttpCheckSnapshot::State::Done && snap->result.status == 200 && snap->step.empty());
        req.url = "https://exa mple.com";
        CHECK(!s.start(req) && s.latest()->state == HttpCheckSnapshot::State::Failed && !s.latest()->error.empty());
        run->block_until_cancel = true;
        req.url = "example.com";
        CHECK(s.start(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(s.latest()->state == HttpCheckSnapshot::State::Running && s.latest()->step == "request");
        const std::int64_t t0 = now_ms();
        s.stop();
        CHECK(s.wait(1000) && now_ms() - t0 < 300);
        snap = s.latest();
        CHECK(snap->state == HttpCheckSnapshot::State::Cancelled && snap->result.problem == HttpProblem::Cancelled);
    }
    // the whole flow through the real runner, with fake curl and openssl programs first in PATH
    {
        const std::string dir = test_tmpdir("fakecurl");
        {
            std::ofstream(dir + "/out-curl") << fixture("curl", "expired.txt");
            std::ofstream(dir + "/out-openssl") << fixture("openssl", "s_client-expired.txt");
            std::ofstream f(dir + "/curl");
            f << "#!/bin/sh\nprintf '%s|' \"$@\" > \"$NT_FAKE_DIR/args-curl\"\ncat \"$NT_FAKE_DIR/out-curl\"\nexit 60\n";
        }
        {
            std::ofstream f(dir + "/openssl");
            f << "#!/bin/sh\nprintf '%s|' \"$@\" > \"$NT_FAKE_DIR/args-openssl\"\ncat \"$NT_FAKE_DIR/out-openssl\"\n";
        }
        chmod((dir + "/curl").c_str(), 0755);
        chmod((dir + "/openssl").c_str(), 0755);
        const char *old_path = std::getenv("PATH");
        const std::string saved = old_path ? old_path : "";
        setenv("PATH", (dir + ":" + saved).c_str(), 1);
        setenv("NT_FAKE_DIR", dir.c_str(), 1);
        HttpCheckSession s(std::make_shared<DirectRunner>(), clock);
        HttpCheckRequest req;
        req.url = "expired.badssl.com";
        CHECK(s.start(req));
        CHECK(s.wait(5000));
        const auto snap = s.latest();
        CHECK(snap->state == HttpCheckSnapshot::State::Done);
        CHECK(snap->result.problem == HttpProblem::CertExpired && snap->result.cert.source == "openssl");
        CHECK(snap->result.cert.not_after == "Apr 12 23:59:59 2015 GMT");
        std::ifstream args(dir + "/args-curl");
        std::string line;
        std::getline(args, line);
        CHECK_EQ(line, std::string("-sS|-v|-o|/dev/null|-g|--proto|=http,https|--proto-redir|=http,https|-m|15|"
                                   "--connect-timeout|8|--max-redirs|5|-L|-w|%{json}\\n|--url|https://expired.badssl.com|"));
        std::ifstream oargs(dir + "/args-openssl");
        std::getline(oargs, line);
        CHECK_EQ(line, std::string("s_client|-connect|expired.badssl.com:443|-servername|expired.badssl.com|"));
        setenv("PATH", saved.c_str(), 1);
        unsetenv("NT_FAKE_DIR");
        remove_dir(dir);
    }
}

} // namespace

void test_httpcheck()
{
    test_minijson();
    test_http_url();
    test_httpcheck_parse();
    test_httpcheck_flow();
}
