/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the scripted answers of the demonstration backend (see demo_data.hpp).
 */
#include "demo_data.hpp"

#include "netaddr.hpp"
#include "text.hpp"

#include <algorithm>

namespace nt {
namespace ui {

namespace {

bool starts(const std::string &s, const char *p)
{
    return s.rfind(p, 0) == 0;
}

/* ---------------------------------------------------------------- traceroute */

TraceHop hop(int ttl, const char *addr, const char *name, int sent, int lost, double avg, double best, double worst, double jitter)
{
    TraceHop h;
    h.ttl = ttl;
    PingStats &st = h.stats;
    st.transmitted = sent;
    st.lost = lost;
    st.received = sent - lost;
    if (st.received > 0) {
        st.min_ms = best;
        st.max_ms = worst;
        st.sum_ms = avg * st.received;
        st.last_ms = avg;
        st.has_last = true;
        st.jitter_sum = jitter;
        st.jitter_n = 1;
    }
    if (addr && addr[0]) {
        TraceHopAddress a;
        a.address = addr;
        a.name = name ? name : "";
        a.count = st.received;
        h.addresses.push_back(a);
    }
    return h;
}

TraceHop silent_hop(int ttl, int sent)
{
    return hop(ttl, nullptr, nullptr, sent, sent, 0, 0, 0, 0);
}

} // namespace

std::time_t demo_now()
{
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    return timegm(&tm);
}

TraceSnapshot demo_trace(const std::string &scenario, const TraceConfig &cfg)
{
    TraceSnapshot s;
    s.target = trim(cfg.target);
    s.max_hops = 30;
    s.method = "ICMP";
    s.rounds = 14;
    s.state = TraceSnapshot::State::Running;
    if (starts(s.target, "nx")) {
        s.state = TraceSnapshot::State::Failed;
        s.error = "Cannot find " + s.target + ": no such name (NXDOMAIN)";
        return s;
    }
    const bool literal = is_ip(s.target);
    s.address = literal ? s.target : "93.184.215.14";
    s.resolved_by = literal ? "" : "192.168.1.1";
    if (scenario == "resolving") {
        s.state = TraceSnapshot::State::Resolving;
        s.address.clear();
        s.rounds = 0;
        return s;
    }
    const char *dest_name = s.target == "1.1.1.1" ? "one.one.one.one" : "";
    int sent = 14;
    std::vector<TraceHop> &h = s.hops;
    const auto path = [&](int upto) { /* the shared first hops, without trouble */
        static const struct {
            const char *addr, *name;
            double avg, best, worst, jitter;
        } p[] = {{"192.168.68.1", "router.lan", 1.3, 1.0, 2.6, 0.3},
                 {"10.20.0.1", "", 7.9, 6.8, 12.4, 1.1},
                 {"62.2.34.1", "gw-wlan.cablecom.example", 11.8, 10.2, 19.0, 1.6},
                 {"195.186.1.9", "ae1.ch-zrh1.example.net", 13.5, 11.9, 20.2, 1.4},
                 {"80.81.192.21", "zrh.ix.example", 15.2, 13.8, 24.5, 1.7},
                 {"129.250.2.3", "ae-1.r20.example.net", 22.4, 20.1, 31.7, 2.2},
                 {"129.250.7.1", "ae-5.a01.example.net", 28.9, 26.3, 40.8, 2.9},
                 {"203.0.113.9", "edge1.cdn.example", 36.1, 33.0, 52.6, 3.4}};
        for (int i = 0; i < upto && i < 8; ++i) h.push_back(hop(i + 1, p[i].addr, p[i].name, sent, 0, p[i].avg, p[i].best, p[i].worst, p[i].jitter));
    };
    const auto finish_reached = [&](int ttl, double avg, double best, double worst, double jitter, int lost) {
        h.push_back(hop(ttl, s.address.c_str(), dest_name, sent, lost, avg, best, worst, jitter));
        s.reached = true;
        s.dest_hop = ttl;
        h.back().destination = true;
    };
    if (scenario == "starting") {
        s.rounds = 0;
        h.push_back(hop(1, "192.168.68.1", "", 1, 0, 1.3, 1.3, 1.3, 0));
        h.push_back(hop(2, "10.20.0.1", "", 1, 0, 8.2, 8.2, 8.2, 0));
        TraceHop pending;
        pending.ttl = 3;
        pending.stats.transmitted = 1;
        h.push_back(pending);
    } else if (scenario == "silent") {
        path(3);
        h.push_back(silent_hop(4, sent));
        for (int i = 4; i < 8; ++i) {
            TraceHop x = hop(i + 1, i == 4 ? "80.81.192.21" : i == 5 ? "129.250.2.3" : i == 6 ? "129.250.7.1" : "203.0.113.9",
                             i == 4 ? "zrh.ix.example" : i == 5 ? "ae-1.r20.example.net" : i == 6 ? "ae-5.a01.example.net" : "edge1.cdn.example", sent, 0,
                             15.2 + (i - 4) * 7.0, 13.8 + (i - 4) * 6.0, 24.5 + (i - 4) * 9.0, 1.7);
            h.push_back(x);
        }
        finish_reached(9, 43.2, 39.5, 61.4, 3.9, 0);
    } else if (scenario == "ratelimit") {
        sent = 30;
        s.rounds = 30;
        path(4);
        h.push_back(hop(5, "80.81.192.21", "zrh.ix.example", sent, 9, 15.2, 13.8, 88.0, 7.5));
        for (int i = 5; i < 8; ++i) {
            static const struct {
                const char *addr, *name;
                double avg, best, worst, jitter;
            } p[] = {{"129.250.2.3", "ae-1.r20.example.net", 22.4, 20.1, 31.7, 2.2},
                     {"129.250.7.1", "ae-5.a01.example.net", 28.9, 26.3, 40.8, 2.9},
                     {"203.0.113.9", "edge1.cdn.example", 36.1, 33.0, 52.6, 3.4}};
            h.push_back(hop(i + 1, p[i - 5].addr, p[i - 5].name, sent, 0, p[i - 5].avg, p[i - 5].best, p[i - 5].worst, p[i - 5].jitter));
        }
        finish_reached(9, 43.2, 39.5, 61.4, 3.9, 0);
    } else if (scenario == "loss") {
        path(5);
        h.push_back(hop(6, "129.250.2.3", "ae-1.r20.example.net", sent, 1, 22.4, 20.1, 31.7, 2.2));
        h.push_back(hop(7, "129.250.7.1", "ae-5.a01.example.net", sent, 3, 61.0, 28.0, 190.0, 38.0));
        h.push_back(hop(8, "203.0.113.9", "edge1.cdn.example", sent, 5, 96.5, 33.0, 310.0, 55.0));
        finish_reached(9, 168.4, 40.0, 420.0, 72.0, 6);
    } else if (scenario == "unreached") {
        path(6);
        for (int i = 7; i <= 12; ++i) h.push_back(silent_hop(i, sent));
    } else if (scenario == "unreach") {
        path(4);
        TraceHop r = hop(5, "80.81.192.21", "zrh.ix.example", sent, 0, 15.2, 13.8, 24.5, 1.7);
        r.unreachable = "host unreachable";
        h.push_back(r);
        s.unreachable = true;
        s.dest_hop = 5;
    } else { /* healthy */
        path(8);
        finish_reached(9, 43.2, 39.5, 61.4, 3.9, 0);
    }
    trace_derive(s);
    return s;
}

/* ---------------------------------------------------------------- HTTP check */

namespace {

CertInfo make_cert(const char *subject, const char *issuer, const char *not_before, const char *not_after, int days_left,
                   const char *source)
{
    CertInfo c;
    c.present = true;
    c.source = source;
    c.subject = subject;
    c.issuer = issuer;
    c.not_before = not_before;
    c.not_after = not_after;
    c.has_days = true;
    c.days_left = days_left;
    c.expired = days_left < 0;
    return c;
}

HttpTimings https_timings()
{
    HttpTimings t;
    t.valid = t.connected = t.tls = t.first_byte = true;
    t.dns_ms = 12;
    t.connect_ms = 18;
    t.tls_ms = 31;
    t.wait_ms = 94;
    t.transfer_ms = 28;
    t.total_ms = 183;
    return t;
}

} // namespace

HttpCheckSnapshot demo_http(const std::string &scenario, const HttpCheckRequest &req)
{
    HttpCheckSnapshot snap;
    snap.input = trim(req.url);
    HttpTarget t;
    std::string why;
    if (!parse_http_target(req.url, t, &why)) {
        snap.state = HttpCheckSnapshot::State::Failed;
        snap.error = "The address cannot be used: " + why;
        return snap;
    }
    HttpCheckResult r;
    r.url = t.url;
    r.head = req.head;
    r.insecure = req.insecure;
    r.reported = true;
    r.answered = true;
    r.status = 200;
    r.reason = "OK";
    r.final_url = t.url + (t.url.find('/', 8) == std::string::npos ? "/" : "");
    r.http_version = t.scheme == "https" ? "HTTP/2" : "HTTP/1.1";
    r.remote_ip = "93.184.215.14";
    r.remote_port = t.port;
    r.size_bytes = req.head ? 0 : 1256;
    r.content_type = "text/html";
    r.timings = https_timings();
    const bool tls = t.scheme == "https";
    if (!tls) {
        r.timings.tls = false;
        r.timings.tls_ms = 0;
        r.timings.total_ms = 152;
    }
    if (tls)
        r.cert = make_cert("CN = example.com", "C = US, O = Let's Encrypt, CN = R11", "Oct  6 08:00:00 2026 GMT",
                           "Jan  6 08:00:00 2027 GMT", 83, "curl");
    const auto fail = [&](HttpProblem p, const std::string &message, bool connected, bool tls_done, double dns, double connect,
                          double tls_ms) {
        r.answered = false;
        r.status = 0;
        r.reason.clear();
        r.final_url.clear();
        r.http_version.clear();
        r.remote_ip.clear();
        r.remote_port = 0;
        r.size_bytes = 0;
        r.problem = p;
        r.curl_message = message;
        r.problem_words = http_problem_words(p, t.host, message);
        r.timings = HttpTimings();
        r.timings.valid = true;
        r.timings.connected = connected;
        r.timings.tls = tls_done;
        r.timings.dns_ms = dns;
        r.timings.connect_ms = connect;
        r.timings.tls_ms = tls_ms;
        r.timings.total_ms = dns + connect + tls_ms;
        r.cert = CertInfo();
    };
    if (scenario == "redirect") {
        r.url = "http://example.com";
        r.final_url = "https://www.example.com/";
        r.redirects = 1;
        r.timings.redirect_ms = 52;
        r.timings.dns_ms = 11;
        r.timings.connect_ms = 17;
        r.timings.tls_ms = 30;
        r.timings.wait_ms = 120;
        r.timings.transfer_ms = 41;
        r.timings.total_ms = 271;
        r.timings.tls = true;
        r.cert = make_cert("CN = www.example.com", "C = US, O = Let's Encrypt, CN = R11", "Oct  6 08:00:00 2026 GMT",
                           "Jan  6 08:00:00 2027 GMT", 83, "curl");
    } else if (scenario == "moved") {
        r.status = 301;
        r.reason = "Moved Permanently";
        r.redirect_url = "https://www.example.com/";
        r.size_bytes = 0;
    } else if (scenario == "404") {
        r.status = 404;
        r.reason = "Not Found";
        r.timings.wait_ms = 61;
        r.timings.total_ms = 150;
    } else if (scenario == "500") {
        r.status = 500;
        r.reason = "Internal Server Error";
        r.size_bytes = 309;
        r.timings.wait_ms = 1240;
        r.timings.total_ms = 1329;
    } else if (scenario == "plain") {
        /* http:// : no TLS, no certificate (the base already took them out) */
    } else if (scenario == "cert5") {
        r.cert.days_left = 5;
        r.cert.not_after = "Oct 20 08:00:00 2026 GMT";
    } else if (scenario == "cert20") {
        r.cert.days_left = 20;
        r.cert.not_after = "Nov  4 08:00:00 2026 GMT";
    } else if (scenario == "e-resolve") {
        fail(HttpProblem::Resolve, "Could not resolve host: " + t.host, false, false, 2003, 0, 0);
        r.timings.valid = false;
    } else if (scenario == "e-refused") {
        fail(HttpProblem::Refused, "Failed to connect: Connection refused", false, false, 12, 0, 0);
    } else if (scenario == "e-timeout") {
        fail(HttpProblem::Timeout, "Connection timed out after 8001 milliseconds", false, false, 14, 0, 0);
        r.timings.total_ms = 8001;
    } else if (scenario == "cert-expired") {
        fail(HttpProblem::CertExpired, "SSL certificate problem: certificate has expired", true, true, 14, 22, 40);
        r.cert = make_cert("CN = *.badssl.com", "C = US, O = Let's Encrypt, CN = R3", "Jul  5 12:00:00 2026 GMT",
                           "Oct  3 12:00:00 2026 GMT", -12, "openssl");
    } else if (scenario == "cert-self") {
        fail(HttpProblem::CertSelfSigned, "SSL certificate problem: self-signed certificate", true, true, 14, 22, 40);
        r.cert = make_cert("C = US, ST = California, L = San Francisco, O = BadSSL, CN = *.badssl.com",
                           "C = US, ST = California, L = San Francisco, O = BadSSL, CN = *.badssl.com", "Feb  3 00:00:00 2026 GMT",
                           "Feb  3 00:00:00 2028 GMT", 475, "openssl");
    } else if (scenario == "cert-mismatch") {
        fail(HttpProblem::CertNameMismatch, "SSL: no alternative certificate subject name matches target host name", true, true, 14, 22,
             40);
        r.cert = make_cert("CN = *.badssl.com", "C = US, O = Let's Encrypt, CN = R3", "Jul  5 12:00:00 2026 GMT",
                           "Oct  3 12:00:00 2027 GMT", 353, "openssl");
    } else if (scenario == "cert-ignored") {
        r.insecure = true;
        r.cert = make_cert("CN = *.badssl.com", "C = US, O = Let's Encrypt, CN = R3", "Jul  5 12:00:00 2026 GMT",
                           "Oct  3 12:00:00 2026 GMT", -12, "openssl");
        r.cert_note = "The certificate has expired: not checked because you asked to ignore certificate errors.";
        r.ssl_verify_result = 10;
    } else if (scenario == "head") {
        r.head = true;
        r.size_bytes = 0;
    }
    if (scenario == "running") {
        snap.state = HttpCheckSnapshot::State::Running;
        snap.step = "request";
        snap.result = HttpCheckResult();
        snap.result.url = t.url;
        return snap;
    }
    snap.state = HttpCheckSnapshot::State::Done;
    snap.result = r;
    return snap;
}

/* ---------------------------------------------------------------- public address */

PublicIpSnapshot demo_pubip(const std::string &scenario)
{
    PublicIpSnapshot s;
    s.state = PublicIpSnapshot::State::Done;
    const auto ok = [](const char *ip, const char *loc, const char *colo) {
        PublicIpFamily f;
        f.tried = true;
        f.ok = true;
        f.trace.ok = true;
        f.trace.ip = ip;
        f.trace.loc = loc;
        f.trace.colo = colo;
        f.trace.http = "http/2";
        f.trace.tls = "TLSv1.3";
        f.trace.warp = "off";
        f.cls = ip_class(std::string(ip));
        return f;
    };
    const auto none = [](const char *why) {
        PublicIpFamily f;
        f.tried = true;
        f.error = why;
        return f;
    };
    if (scenario == "running") {
        s.state = PublicIpSnapshot::State::Running;
        s.family = 4;
        return s;
    }
    if (scenario == "offline") {
        s.result.v4 = none("no connection to Cloudflare");
        s.result.v6 = none("no IPv6");
    } else if (scenario == "v4") {
        s.result.v4 = ok("203.0.113.7", "CH", "ZRH");
        s.result.v6 = none("no IPv6");
    } else if (scenario == "cgnat") {
        s.result.v4 = ok("198.51.100.23", "DE", "FRA");
        s.result.v6 = none("no IPv6");
    } else {
        s.result.v4 = ok("203.0.113.7", "CH", "ZRH");
        s.result.v6 = ok("2001:db8:1f2:a3::7", "CH", "ZRH");
    }
    return s;
}

/* ---------------------------------------------------------------- whois */

namespace {

const char *kIanaDomain = R"(% IANA WHOIS server
% for more information on IANA, visit http://www.iana.org
% This query returned 1 object

refer:        whois.verisign-grs.com

domain:       COM

organisation: VeriSign Global Registry Services
address:      12061 Bluemont Way
address:      Reston VA 20190
address:      United States of America (the)

whois:        whois.verisign-grs.com

status:       ACTIVE
remarks:      Registration information: http://www.verisign-grs.com

created:      1985-01-01
changed:      2023-12-07
source:       IANA
)";

const char *kVerisignExample = R"(   Domain Name: EXAMPLE.COM
   Registry Domain ID: 2336799_DOMAIN_COM-VRSN
   Registrar WHOIS Server: whois.iana.org
   Registrar URL: http://res-dom.iana.org
   Updated Date: 2026-08-14T07:01:31Z
   Creation Date: 1995-08-14T04:00:00Z
   Registry Expiry Date: 2027-01-06T04:00:00Z
   Registrar: RESERVED-Internet Assigned Numbers Authority
   Registrar IANA ID: 376
   Registrar Abuse Contact Email:
   Registrar Abuse Contact Phone:
   Domain Status: clientDeleteProhibited https://icann.org/epp#clientDeleteProhibited
   Domain Status: clientTransferProhibited https://icann.org/epp#clientTransferProhibited
   Domain Status: clientUpdateProhibited https://icann.org/epp#clientUpdateProhibited
   Name Server: A.IANA-SERVERS.NET
   Name Server: B.IANA-SERVERS.NET
   DNSSEC: signedDelegation
   URL of the ICANN Whois Inaccuracy Complaint Form: https://www.icann.org/wicf/
>>> Last update of whois database: 2026-10-15T12:06:25Z <<<

For more information on Whois status codes, please visit https://icann.org/epp
)";

const char *kVerisignGoogle = R"(   Domain Name: GOOGLE.COM
   Registry Domain ID: 2138514_DOMAIN_COM-VRSN
   Registrar WHOIS Server: whois.markmonitor.com
   Registrar URL: http://www.markmonitor.com
   Updated Date: 2019-09-09T15:39:04Z
   Creation Date: 1997-09-15T04:00:00Z
   Registry Expiry Date: 2028-09-14T04:00:00Z
   Registrar: MarkMonitor Inc.
   Registrar IANA ID: 292
   Registrar Abuse Contact Email: abusecomplaints@markmonitor.com
   Registrar Abuse Contact Phone: +1.2086851750
   Domain Status: clientDeleteProhibited https://icann.org/epp#clientDeleteProhibited
   Domain Status: clientTransferProhibited https://icann.org/epp#clientTransferProhibited
   Domain Status: clientUpdateProhibited https://icann.org/epp#clientUpdateProhibited
   Domain Status: serverDeleteProhibited https://icann.org/epp#serverDeleteProhibited
   Domain Status: serverTransferProhibited https://icann.org/epp#serverTransferProhibited
   Domain Status: serverUpdateProhibited https://icann.org/epp#serverUpdateProhibited
   Name Server: NS1.GOOGLE.COM
   Name Server: NS2.GOOGLE.COM
   Name Server: NS3.GOOGLE.COM
   Name Server: NS4.GOOGLE.COM
   DNSSEC: unsigned
   URL of the ICANN Whois Inaccuracy Complaint Form: https://www.icann.org/wicf/
>>> Last update of whois database: 2026-10-15T12:06:25Z <<<

For more information on Whois status codes, please visit https://icann.org/epp

NOTICE: The expiration date displayed in this record is the date the
registrar's sponsorship of the domain name registration in the registry is
currently set to expire. This date does not necessarily reflect the expiration
date of the domain name registrant's agreement with the sponsoring
registrar.  Users may consult the sponsoring registrar's Whois database to
view the registrar's reported date of expiration for this registration.

TERMS OF USE: You are not authorized to access or query our Whois
database through the use of electronic processes that are high-volume and
automated except as reasonably necessary to register domain names or
modify existing registrations; the Data in VeriSign Global Registry
Services' ("VeriSign") Whois database is provided by VeriSign for
information purposes only, and to assist persons in obtaining information
about or related to a domain name registration record. VeriSign does not
guarantee its accuracy. By submitting a Whois query, you agree to abide
by the following terms of use: You agree that you may use this Data only
for lawful purposes and that under no circumstances will you use this Data
to: (1) allow, enable, or otherwise support the transmission of mass
unsolicited, commercial advertising or solicitations via e-mail, telephone,
or facsimile; or (2) enable high volume, automated, electronic processes
that apply to VeriSign (or its computer systems). The compilation,
repackaging, dissemination or other use of this Data is expressly
prohibited without the prior written consent of VeriSign. You agree not to
use electronic processes that are automated and high-volume to access or
query the Whois database except as reasonably necessary to register
domain names or modify existing registrations. VeriSign reserves the right
to restrict your access to the Whois database in its sole discretion to ensure
operational stability.  VeriSign may restrict or terminate your access to the
Whois database for failure to abide by these terms of use. VeriSign
reserves the right to modify these terms at any time.

The Registry database contains ONLY .COM, .NET, .EDU domains and
Registrars.
)";

const char *kMarkMonitor = R"(Domain Name: google.com
Registry Domain ID: 2138514_DOMAIN_COM-VRSN
Registrar WHOIS Server: whois.markmonitor.com
Registrar URL: http://www.markmonitor.com
Updated Date: 2024-08-02T02:17:33+0000
Creation Date: 1997-09-15T07:00:00+0000
Registrar Registration Expiration Date: 2028-09-13T07:00:00+0000
Registrar: MarkMonitor, Inc.
Registrar IANA ID: 292
Registrar Abuse Contact: https://corp.markmonitor.com/domain/ui/abuse-report
Registrar Abuse Contact Phone: +1.2086851750
Domain Status: clientUpdateProhibited (https://www.icann.org/epp#clientUpdateProhibited)
Domain Status: clientTransferProhibited (https://www.icann.org/epp#clientTransferProhibited)
Domain Status: clientDeleteProhibited (https://www.icann.org/epp#clientDeleteProhibited)
Registrant Organization: Google LLC
Registrant Country: US
Registrant Email: Select Request Email Form at https://domains.markmonitor.com/whois/google.com
Tech Email: Select Request Email Form at https://domains.markmonitor.com/whois/google.com
Name Server: ns3.google.com
Name Server: ns2.google.com
Name Server: ns1.google.com
Name Server: ns4.google.com
DNSSEC: unsigned
URL of the ICANN WHOIS Data Problem Reporting System: http://wdprs.internic.net/
>>> Last update of WHOIS database: 2026-10-15T12:03:46+0000 <<<

For more information on WHOIS status codes, please visit:
  https://www.icann.org/resources/pages/epp-status-codes

If you wish to contact this domain's Registrant or Technical
contact, and such email address is not visible above, you may do so via our web
form, pursuant to ICANN's Temporary Specification. To verify that you are not a
robot, please enter your email address to receive a link to a page that
facilitates email communication with the relevant contact(s).

Web-based WHOIS:
  https://domains.markmonitor.com/whois/contact/google.com
)";

const char *kIanaArin = R"(% IANA WHOIS server
% for more information on IANA, visit http://www.iana.org
% This query returned 1 object

refer:        whois.arin.net

inetnum:      8.0.0.0 - 8.255.255.255
organisation: Level 3 Parent, LLC
status:       LEGACY

whois:        whois.arin.net

changed:      1992-12
source:       IANA
)";

const char *kArin = R"(NetRange:       8.8.8.0 - 8.8.8.255
CIDR:           8.8.8.0/24
NetName:        GOGL
NetHandle:      NET-8-8-8-0-2
Parent:         NET8 (NET-8-0-0-0-0)
NetType:        Direct Allocation
OriginAS:       AS15169
Organization:   Google LLC (GOGL)
RegDate:        2023-12-28
Updated:        2023-12-28
Ref:            https://rdap.arin.net/registry/ip/8.8.8.0

OrgName:        Google LLC
OrgId:          GOGL
Address:        1600 Amphitheatre Parkway
City:           Mountain View
StateProv:      CA
PostalCode:     94043
Country:        US
RegDate:        2000-03-30
Updated:        2019-10-31
Ref:            https://rdap.arin.net/registry/entity/GOGL

OrgTechHandle: ZG39-ARIN
OrgTechName:   Google LLC
OrgTechPhone:  +1-650-253-0000
OrgTechEmail:  arin-contact@google.com

OrgAbuseHandle: ABUSE5250-ARIN
OrgAbuseName:   Abuse
OrgAbusePhone:  +1-650-253-0000
OrgAbuseEmail:  network-abuse@google.com
OrgAbuseRef:    https://rdap.arin.net/registry/entity/ABUSE5250-ARIN
)";

const char *kIanaRipe = R"(% IANA WHOIS server
% for more information on IANA, visit http://www.iana.org
% This query returned 1 object

refer:        whois.ripe.net

inetnum:      193.0.0.0 - 193.255.255.255
organisation: RIPE NCC
status:       ALLOCATED

whois:        whois.ripe.net

changed:      1993-05
source:       IANA
)";

const char *kRipe = R"(% This is the RIPE Database query service.
% The objects are in RPSL format.
%
% Information related to '193.0.0.0 - 193.0.7.255'

% Abuse contact for '193.0.0.0 - 193.0.7.255' is 'abuse@ripe.net'

inetnum:        193.0.0.0 - 193.0.7.255
netname:        RIPE-NCC
descr:          RIPE Network Coordination Centre
org:            ORG-RIEN1-RIPE
descr:          Amsterdam, Netherlands
remarks:        Used for RIPE NCC infrastructure.
country:        NL
admin-c:        MDIR-RIPE
tech-c:         OPS4-RIPE
status:         ASSIGNED PA
mnt-by:         RIPE-NCC-MNT
created:        2003-03-17T12:15:57Z
last-modified:  2026-03-19T09:08:35Z
source:         RIPE

organisation:   ORG-RIEN1-RIPE
org-name:       Reseaux IP Europeens Network Coordination Centre (RIPE NCC)
country:        NL
org-type:       LIR
address:        P.O. Box 10096
address:        1001 EB
address:        Amsterdam
address:        NETHERLANDS
abuse-c:        OPS4-RIPE

route:          193.0.0.0/21
descr:          RIPE-NCC
origin:         AS3333
mnt-by:         RIPE-NCC-MNT
created:        2018-11-22T15:27:19Z
source:         RIPE
)";

const char *kNoMatch = R"(No match for "WWW.GOOGLE.COM".
>>> Last update of whois database: 2026-10-15T12:06:25Z <<<

NOTICE: The expiration date displayed in this record is the date the
registrar's sponsorship of the domain name registration in the registry is
currently set to expire.
)";

WhoisAnswer answer(const char *server, const std::string &query, const char *text, bool is_ip)
{
    WhoisAnswer a;
    a.server = server;
    a.query = query;
    a.ok = true;
    a.lines = split_lines(text);
    a.bytes = std::string(text).size();
    a.summary = parse_whois_summary(a.lines, is_ip);
    return a;
}

} // namespace

WhoisSnapshot demo_whois(const std::string &scenario, const WhoisRequest &req)
{
    WhoisSnapshot snap;
    snap.input = trim(req.query);
    std::string object, why;
    bool is_ip = false;
    if (!parse_whois_query(req.query, object, is_ip, &why)) {
        snap.state = WhoisSnapshot::State::Failed;
        snap.error = why;
        return snap;
    }
    WhoisResult r;
    r.object = object;
    r.is_ip = is_ip;
    if (scenario == "running") {
        snap.state = WhoisSnapshot::State::Running;
        snap.asking = kWhoisIana;
        snap.result = r;
        return snap;
    }
    if (scenario == "offline") {
        WhoisAnswer a;
        a.server = kWhoisIana;
        a.query = object;
        a.error = "Cannot connect to whois.iana.org: the network is unreachable";
        r.answers.push_back(a);
    } else if (scenario == "arin" || (is_ip && scenario != "ripe")) {
        r.answers.push_back(answer(kWhoisIana, object, kIanaArin, true));
        r.answers.push_back(answer("whois.arin.net", "n + " + object, kArin, true));
    } else if (scenario == "ripe") {
        r.answers.push_back(answer(kWhoisIana, object, kIanaRipe, true));
        r.answers.push_back(answer("whois.ripe.net", object, kRipe, true));
    } else if (scenario == "notfound") {
        r.answers.push_back(answer(kWhoisIana, object, kIanaDomain, false));
        r.answers.push_back(answer("whois.verisign-grs.com", object, kNoMatch, false));
    } else if (scenario == "long") {
        r.answers.push_back(answer(kWhoisIana, object, kIanaDomain, false));
        r.answers.push_back(answer("whois.verisign-grs.com", object, kVerisignGoogle, false));
        r.answers.push_back(answer("whois.markmonitor.com", object, kMarkMonitor, false));
    } else { /* domain */
        r.answers.push_back(answer(kWhoisIana, object, kIanaDomain, false));
        r.answers.push_back(answer("whois.verisign-grs.com", object, kVerisignExample, false));
    }
    r.summary = whois_summary_of(r);
    snap.state = WhoisSnapshot::State::Done;
    snap.result = r;
    return snap;
}

} // namespace ui
} // namespace nt
