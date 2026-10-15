/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the saved text of the 0.2 tools (traceroute, HTTP check, public address, whois).
 */
#include "check.hpp"

#include "netlog.hpp"

using namespace nt;

namespace {

PingStats stats(int sent, std::initializer_list<double> rtts, int lost)
{
    PingStats s;
    for (int i = 0; i < sent; ++i) s.add_sent();
    for (const double r : rtts) s.add_reply(r);
    for (int i = 0; i < lost; ++i) s.add_lost();
    return s;
}

} // namespace

void test_netlog_02()
{
    // traceroute: the summary rows, a silent hop, rate limiting, a second address, the hops cut off
    {
        TraceSnapshot s;
        s.state = TraceSnapshot::State::Done;
        s.target = "example.com";
        s.address = "93.184.215.14";
        s.resolved_by = "192.168.1.1";
        s.method = "UDP";
        s.method_note = "ICMP sockets are not allowed here (not allowed): UDP probes instead";
        s.rounds = 4;
        s.max_hops = 30;
        s.hops.resize(5);
        for (int i = 0; i < 5; ++i) s.hops[static_cast<std::size_t>(i)].ttl = i + 1;
        s.hops[0].stats = stats(4, {1, 2, 1, 2}, 0);
        s.hops[0].addresses = {{"192.168.68.1", "router.lan", 4}};
        s.hops[1].stats = stats(4, {}, 4);
        s.hops[2].stats = stats(4, {10, 12}, 2);
        s.hops[2].addresses = {{"10.0.0.1", "", 1}, {"10.0.0.2", "core-b.isp", 1}};
        s.hops[3].stats = stats(4, {20, 20, 20, 20}, 0);
        s.hops[3].addresses = {{"10.0.1.1", "", 4}};
        s.hops[4].stats = stats(4, {}, 4);
        trace_derive(s);
        std::string t = format_trace(s);
        CHECK(t.find("Target: example.com (93.184.215.14)\nResolved by: 192.168.1.1\nProbes: UDP (ICMP sockets") == 0);
        CHECK(t.find("Rounds: 4\nDestination not reached (30 hops at most)\n") != std::string::npos);
        CHECK(t.find(" 1  192.168.68.1 (router.lan)  0%  4  2.0 ms  1.5 ms  1.0 ms  2.0 ms  1.0 ms\n") != std::string::npos);
        CHECK(t.find(" 2  no answer (silent hop)\n") != std::string::npos);
        CHECK(t.find(" 3  10.0.0.1  50%  4") != std::string::npos);
        CHECK(t.find("    loss only at this hop: likely ICMP rate limiting, not a real problem\n") != std::string::npos);
        CHECK(t.find("    also 10.0.0.2 (core-b.isp), 1 answers\n") != std::string::npos);
        CHECK(t.find("Hops 5 to 5 did not answer\n") != std::string::npos);
        s.reached = true;
        s.dest_hop = 4;
        s.hops.resize(4);
        s.hops[3].destination = true;
        trace_derive(s);
        t = format_trace(s);
        CHECK(t.find("Destination reached at hop 4\n") != std::string::npos);
        CHECK(t.find("  20 ms  20 ms  20 ms  0.0 ms  destination\n") != std::string::npos);
        CHECK(t.find("did not answer") == std::string::npos);
    }
    // HTTP check: the waterfall, the certificate, the problem
    {
        HttpCheckResult r;
        r.url = "https://example.com";
        r.answered = true;
        r.status = 200;
        r.reason = "OK";
        r.http_version = "HTTP/2";
        r.final_url = "https://example.com/";
        r.remote_ip = "172.66.147.243";
        r.remote_port = 443;
        r.size_bytes = 577;
        r.timings.valid = r.timings.tls = r.timings.connected = r.timings.first_byte = true;
        r.timings.dns_ms = 39.6;
        r.timings.connect_ms = 15.4;
        r.timings.tls_ms = 143.9;
        r.timings.wait_ms = 81.4;
        r.timings.transfer_ms = 0.4;
        r.timings.total_ms = 281.4;
        r.cert.present = r.cert.has_days = true;
        r.cert.source = "curl";
        r.cert.subject = "CN = example.com";
        r.cert.issuer = "C = US, O = SSL Corporation";
        r.cert.not_before = "Sep 26 22:49:11 2026 GMT";
        r.cert.not_after = "Dec 25 22:56:35 2026 GMT";
        r.cert.days_left = 71;
        std::string t = format_httpcheck(r);
        CHECK(t.find("URL: https://example.com\nStatus: 200 OK, HTTP/2\nServer: 172.66.147.243 port 443\nSize: 577 bytes\n") ==
              0);
        CHECK(t.find("Timing: DNS 40 ms, connect 15 ms, TLS 144 ms, wait 81 ms, transfer 0.4 ms, total 281 ms\n") !=
              std::string::npos);
        CHECK(t.find("Certificate: CN = example.com\n  issued by C = US, O = SSL Corporation\n  valid from Sep 26 22:49:11 "
                     "2026 GMT to Dec 25 22:56:35 2026 GMT\n  71 days left\n  (read by curl)\n") != std::string::npos);
        CHECK(t.find("Problem") == std::string::npos);
        HttpCheckResult bad;
        bad.url = "https://expired.badssl.com";
        bad.problem = HttpProblem::CertExpired;
        bad.problem_words = "The server's certificate has expired.";
        bad.curl_message = "SSL certificate problem: certificate has expired";
        bad.cert.present = bad.cert.has_days = bad.cert.expired = true;
        bad.cert.source = "openssl";
        bad.cert.subject = "CN=*.badssl.com";
        bad.cert.days_left = -4204;
        t = format_httpcheck(bad);
        CHECK(t.find("  expired 4204 days ago\n  (read by openssl)\n") != std::string::npos);
        CHECK(t.find("Problem: The server's certificate has expired.\ncurl: SSL certificate problem") != std::string::npos);
    }
    // the public address
    {
        PublicIpResult r;
        r.v4.tried = r.v4.ok = true;
        r.v4.trace.ip = "1.0.0.1";
        r.v4.trace.loc = "FR";
        r.v4.trace.colo = "CDG";
        r.v4.cls = IpClass::Public;
        r.v6.tried = true;
        r.v6.error = "no IPv6";
        std::string t = format_publicip(r, "192.168.68.106/22");
        CHECK_EQ(t, std::string("IPv4: 1.0.0.1 (public), country FR, Cloudflare site CDG\nIPv6: no IPv6\n"
                                "Board address: 192.168.68.106/22 (behind a router: NAT)\n"));
        t = format_publicip(r, "100.72.3.4");
        CHECK(t.find("(behind carrier-grade NAT") != std::string::npos);
        CHECK(format_publicip(PublicIpResult()).find("IPv4: not asked\n") == 0);
    }
    // whois: the summary, then each server's lines
    {
        WhoisResult r;
        r.object = "8.8.8.8";
        r.is_ip = true;
        WhoisAnswer a;
        a.server = "whois.iana.org";
        a.query = "8.8.8.8";
        a.ok = true;
        a.lines = {"refer: whois.arin.net"};
        WhoisAnswer b;
        b.server = "whois.arin.net";
        b.query = "n + 8.8.8.8";
        b.ok = true;
        b.lines = {"NetName: GOGL", "CIDR: 8.8.8.0/24"};
        r.answers = {a, b};
        r.summary.netname = "GOGL";
        r.summary.cidr = "8.8.8.0/24";
        r.summary.abuse = "network-abuse@google.com";
        const std::string t = format_whois(r);
        CHECK_EQ(t, std::string("Query: 8.8.8.8 (address)\nNetwork: GOGL\nRange: 8.8.8.0/24\nAbuse contact: "
                                "network-abuse@google.com\n--- whois.iana.org (asked \"8.8.8.8\")\nrefer: whois.arin.net\n"
                                "--- whois.arin.net (asked \"n + 8.8.8.8\")\nNetName: GOGL\nCIDR: 8.8.8.0/24\n"));
        WhoisResult d;
        d.object = "google.com";
        d.summary.registrar = "MarkMonitor, Inc.";
        d.summary.status = {"clientDeleteProhibited", "serverDeleteProhibited"};
        d.summary.name_servers = {"ns1.google.com", "ns2.google.com"};
        d.note = "referral limit reached (x not asked)";
        WhoisAnswer fail;
        fail.server = "whois.verisign-grs.com";
        fail.query = "google.com";
        fail.error = "timed out";
        d.answers = {fail};
        d.lines_truncated = true;
        const std::string u = format_whois(d);
        CHECK(u.find("Query: google.com (domain)\nRegistrar: MarkMonitor, Inc.\nStatus: clientDeleteProhibited "
                     "serverDeleteProhibited\nName servers: ns1.google.com ns2.google.com\nNote: referral limit") == 0);
        CHECK(u.find("--- whois.verisign-grs.com (asked \"google.com\"): timed out\n--- (cut at 400 lines)\n") !=
              std::string::npos);
    }
}
