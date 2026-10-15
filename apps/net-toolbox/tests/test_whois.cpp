/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: whois. The query rules, the referral and summary parsers on answers recorded from the real servers
 * (tests/fixtures/whois, 2026-10-15: IANA for a domain, IPv4 in ARIN and RIPE space, IPv6, a private range and
 * example.com; Verisign for google.com and for www.google.com (no match); MarkMonitor; ARIN for 8.8.8.8, for an IPv6
 * address and for RIPE space (ReferralServer); RIPE; APNIC), the line cleaning; then the client against fake whois
 * servers on loopback (127.0.0.2 .. on one port, named in a hosts file): the referral chains, ARIN's "n +" query, the
 * parent-domain retry, a referral loop, the referral limit, an oversized answer, garbage bytes, a silent server, a
 * refused connection, an unknown server name, the session and its cancel.
 */
#include "check.hpp"

#include "whois.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using namespace nt;

namespace {

std::vector<std::string> lines_of(const char *name)
{
    return whois_clean_lines(fixture("whois", name), SIZE_MAX);
}

bool valid_utf8_printable(const std::string &s)
{
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

/* A whois server on ip:port: reads one line, answers from its table (or the default), closes. */
class FakeWhois {
public:
    enum class Mode { Answer, Endless, Silent };
    FakeWhois(const char *ip, std::uint16_t port) : ip_(ip)
    {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        if (bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(fd_, 8) != 0) {
            close(fd_);
            fd_ = -1;
            return;
        }
        socklen_t len = sizeof(a);
        getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~FakeWhois()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
    }
    bool ok() const { return fd_ >= 0; }
    std::uint16_t port() const { return port_; }
    void answer(const std::string &query, const std::string &text)
    {
        std::lock_guard<std::mutex> lock(mu_);
        table_[query] = text;
    }
    void fallback(const std::string &text)
    {
        std::lock_guard<std::mutex> lock(mu_);
        default_ = text;
    }
    std::vector<std::string> queries() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return queries_;
    }
    std::atomic<int> mode{static_cast<int>(Mode::Answer)};

private:
    void loop()
    {
        while (!stop_) {
            pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            const int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) continue;
            timeval tv{1, 0};
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string q;
            char buf[256];
            while (q.find('\n') == std::string::npos && q.size() < 1024) {
                const ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                q.append(buf, static_cast<std::size_t>(n));
            }
            while (!q.empty() && (q.back() == '\n' || q.back() == '\r')) q.pop_back();
            std::string reply;
            {
                std::lock_guard<std::mutex> lock(mu_);
                queries_.push_back(q);
                const auto it = table_.find(q);
                reply = it != table_.end() ? it->second : default_;
            }
            const Mode m = static_cast<Mode>(mode.load());
            if (m == Mode::Silent) {
                while (!stop_ && recv(c, buf, sizeof(buf), 0) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else if (m == Mode::Endless) {
                const std::string chunk(4096, 'x');
                std::string line = "remarks: " + chunk + "\n";
                for (int i = 0; i < 2000 && !stop_; ++i)
                    if (send(c, line.data(), line.size(), MSG_NOSIGNAL) < 0) break;
            } else {
                send(c, reply.data(), reply.size(), MSG_NOSIGNAL);
            }
            close(c);
        }
    }
    std::string ip_;
    int fd_ = -1;
    std::uint16_t port_ = 0;
    mutable std::mutex mu_;
    std::map<std::string, std::string> table_;
    std::string default_;
    std::vector<std::string> queries_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

void test_whois_parsers()
{
    std::string obj;
    bool ip = false;
    std::string why;
    CHECK(parse_whois_query(" Example.COM. ", obj, ip) && obj == "example.com" && !ip);
    CHECK(parse_whois_query("8.8.8.8", obj, ip) && obj == "8.8.8.8" && ip);
    CHECK(parse_whois_query("2001:4860:4860:0:0:0:0:8888", obj, ip) && obj == "2001:4860:4860::8888" && ip);
    for (const char *bad : {"", "  ", "-h", "localhost", "exa mple.com", "a;b.com", "x.com\r\nhelp", "fe80::1%eth0",
                            "a..com", "http://example.com"}) {
        why.clear();
        CHECK(!parse_whois_query(bad, obj, ip, &why) && !why.empty());
    }
    CHECK_EQ(whois_query_text("whois.arin.net", "8.8.8.8", true), std::string("n + 8.8.8.8"));
    CHECK_EQ(whois_query_text("WHOIS.ARIN.NET", "2001:db8::1", true), std::string("n + 2001:db8::1"));
    CHECK_EQ(whois_query_text("whois.ripe.net", "193.0.6.139", true), std::string("193.0.6.139"));
    CHECK_EQ(whois_query_text("whois.verisign-grs.com", "google.com", false), std::string("google.com"));

    // referrals in the recorded answers
    CHECK_EQ(whois_referral(lines_of("iana-google.txt"), "whois.iana.org"), std::string("whois.verisign-grs.com"));
    CHECK_EQ(whois_referral(lines_of("iana-ipv4-arin.txt"), "whois.iana.org"), std::string("whois.arin.net"));
    CHECK_EQ(whois_referral(lines_of("iana-ipv4-ripe.txt"), "whois.iana.org"), std::string("whois.ripe.net"));
    CHECK_EQ(whois_referral(lines_of("iana-ipv6.txt"), "whois.iana.org"), std::string("whois.arin.net")); /* whois: */
    CHECK_EQ(whois_referral(lines_of("iana-private.txt"), "whois.iana.org"), std::string(""));
    CHECK_EQ(whois_referral(lines_of("iana-domain.txt"), "whois.iana.org"), std::string("")); /* IANA's own */
    CHECK_EQ(whois_referral(lines_of("verisign-google.txt"), "whois.verisign-grs.com"),
             std::string("whois.markmonitor.com"));
    CHECK_EQ(whois_referral(lines_of("markmonitor-google.txt"), "whois.markmonitor.com"), std::string("")); /* itself */
    CHECK_EQ(whois_referral(lines_of("arin-ripe-space.txt"), "whois.arin.net"), std::string("whois.ripe.net"));
    CHECK_EQ(whois_referral(lines_of("arin-8.8.8.8.txt"), "whois.arin.net"), std::string(""));
    CHECK_EQ(whois_referral({"ReferralServer:  whois://whois.apnic.net"}, "whois.arin.net"), std::string("whois.apnic.net"));
    CHECK_EQ(whois_referral({"ReferralServer: rwhois://rwhois.example.net:4321"}, "x"), std::string(""));
    CHECK_EQ(whois_referral({"ReferralServer: whois://whois.example.net:43/"}, "x"), std::string("whois.example.net"));
    CHECK_EQ(whois_referral({"refer: https://evil.example/"}, "x"), std::string(""));
    CHECK_EQ(whois_referral({"refer: 10.0.0.1"}, "x"), std::string(""));
    CHECK_EQ(whois_referral({"refer: ; rm -rf /"}, "x"), std::string(""));
    CHECK_EQ(whois_referral({"Registrar WHOIS Server:", "refer: a.example"}, "x"), std::string("a.example"));

    // summaries
    WhoisSummary s = parse_whois_summary(lines_of("verisign-google.txt"), false);
    CHECK(s.registrar == "MarkMonitor Inc." && s.created == "1997-09-15T04:00:00Z" && s.updated == "2019-09-09T15:39:04Z");
    CHECK_EQ(s.expires, std::string("2028-09-14T04:00:00Z"));
    CHECK(s.status.size() == 6 && s.status[0] == "clientDeleteProhibited");
    CHECK(s.name_servers == std::vector<std::string>({"ns1.google.com", "ns2.google.com", "ns3.google.com", "ns4.google.com"}));
    CHECK(s.netname.empty() && s.cidr.empty());
    s = parse_whois_summary(lines_of("markmonitor-google.txt"), false);
    CHECK(s.registrar == "MarkMonitor, Inc." && s.expires == "2028-09-13T07:00:00+0000" && s.status.size() == 6);
    CHECK(s.status[0] == "clientUpdateProhibited" && s.name_servers.size() == 4);
    s = parse_whois_summary(lines_of("verisign-nomatch.txt"), false);
    CHECK(s.empty() && whois_not_found(lines_of("verisign-nomatch.txt")));
    CHECK(!whois_not_found(lines_of("verisign-google.txt")));
    s = parse_whois_summary(lines_of("arin-8.8.8.8.txt"), true);
    CHECK(s.netname == "GOGL" && s.organisation == "Google LLC" && s.country == "US" && s.cidr == "8.8.8.0/24");
    CHECK(s.abuse == "network-abuse@google.com" && s.as_number.empty()); /* OriginAS is empty there: not guessed */
    s = parse_whois_summary(lines_of("arin-ipv6.txt"), true);
    CHECK(s.netname == "GOOGLE-IPV6" && s.cidr == "2001:4860::/32" && s.organisation == "Google LLC");
    s = parse_whois_summary(lines_of("ripe-193.0.6.139.txt"), true);
    CHECK(s.netname == "RIPE-NCC" && s.country == "NL" && s.cidr == "193.0.0.0 - 193.0.7.255");
    CHECK_EQ(s.organisation, std::string("Reseaux IP Europeens Network Coordination Centre (RIPE NCC)"));
    CHECK_EQ(s.abuse, std::string("abuse@ripe.net"));
    s = parse_whois_summary(lines_of("apnic-1.1.1.1.txt"), true);
    CHECK(s.netname == "APNIC-LABS" && s.country == "AU" && s.cidr == "1.1.1.0 - 1.1.1.255");
    CHECK(s.abuse == "helpdesk@apnic.net" && !s.organisation.empty());
    s = parse_whois_summary(lines_of("iana-private.txt"), true);
    CHECK(s.organisation == "IANA - Private Use" && s.cidr == "10.0.0.0 - 10.255.255.255" && s.netname.empty());
    s = parse_whois_summary({"aut-num: 13335", "origin: AS3215", "organisation: ORG-RIEN1-RIPE"}, true);
    CHECK(s.as_number == "AS3215" && s.organisation.empty()); /* the last AS; an ORG- handle is not a name */
    s = parse_whois_summary({"aut-num: N/A"}, true);
    CHECK(s.as_number.empty());
    /* the ".uk" style: the value under its key */
    s = parse_whois_summary({"    Registrar:", "        Markmonitor Inc. t/a MarkMonitor Inc. [Tag = MARKMONITOR]",
                             "    Name servers:", "        ns1.google.com", "        ns2.google.com.", "",
                             "    Expiry date:  14-Feb-2027"},
                            false);
    CHECK(s.registrar == "Markmonitor Inc. t/a MarkMonitor Inc. [Tag = MARKMONITOR]" && s.expires == "14-Feb-2027");
    CHECK(s.name_servers == std::vector<std::string>({"ns1.google.com", "ns2.google.com"}));

    // cleaning: CR, tabs, control bytes, bad UTF-8, long lines, blank runs
    bool cut = false;
    const std::string raw = std::string("\r\n\r\nkey:\tvalue\r\n\x01\x1b[31mred\x07\r\n\r\n\r\n") + "caf\xC3\xA9 \xFF\xFE ok" +
                            "\nover\xC0\xAF" "long \xED\xA0\x80 surrogate \xF4\x90\x80\x80 big\n" + std::string(1000, 'z') +
                            "\nlast\n";
    const std::vector<std::string> c = whois_clean_lines(raw, SIZE_MAX, &cut);
    CHECK(!cut && c.size() == 7);
    if (c.size() == 7) {
        CHECK_EQ(c[0], std::string("key: value"));
        CHECK_EQ(c[1], std::string("[31mred"));
        CHECK_EQ(c[2], std::string(""));
        CHECK_EQ(c[3], std::string("caf\xC3\xA9 ?? ok"));
        CHECK_EQ(c[4], std::string("over??long ??? surrogate ???? big"));
        CHECK(c[5].size() == kWhoisMaxLineBytes);
        CHECK_EQ(c[6], std::string("last"));
    }
    const std::vector<std::string> few = whois_clean_lines("a\nb\nc\nd\n", 2, &cut);
    CHECK(few.size() == 2 && cut);
}

void test_whois_client()
{
    FakeWhois iana("127.0.0.2", 0);
    if (!iana.ok()) {
        SKIP("cannot listen on 127.0.0.2: the whois client is not tested on loopback");
        return;
    }
    const std::uint16_t port = iana.port();
    FakeWhois verisign("127.0.0.3", port), markmonitor("127.0.0.4", port), arin("127.0.0.5", port),
        ripe("127.0.0.6", port), loop_a("127.0.0.7", port), loop_b("127.0.0.8", port), loop_c("127.0.0.9", port),
        loop_d("127.0.0.10", port);
    if (!verisign.ok() || !markmonitor.ok() || !arin.ok() || !ripe.ok() || !loop_a.ok() || !loop_b.ok() ||
        !loop_c.ok() || !loop_d.ok()) {
        SKIP("cannot listen on 127.0.0.3 .. 127.0.0.10 with one port: the whois client is not tested on loopback");
        return;
    }
    const std::string dir = test_tmpdir("whois");
    std::ofstream(dir + "/hosts") << "127.0.0.2 whois.iana.org\n127.0.0.3 whois.verisign-grs.com\n"
                                     "127.0.0.4 whois.markmonitor.com\n127.0.0.5 whois.arin.net\n"
                                     "127.0.0.6 whois.ripe.net\n127.0.0.7 loop-a.example\n127.0.0.8 loop-b.example\n"
                                     "127.0.0.9 loop-c.example\n127.0.0.10 loop-d.example\n"
                                     "127.0.0.11 nobody.example\n";
    WhoisRequest base;
    base.resolve.hosts_path = dir + "/hosts";
    base.resolve.server = ""; /* names only from the hosts file */
    base.port = port;
    base.total_ms = 5000;
    const auto run = [&](const char *query) {
        WhoisRequest req = base;
        req.query = query;
        return run_whois(req, CancelToken());
    };
    iana.answer("google.com", fixture("whois", "iana-google.txt"));
    iana.answer("www.google.com", fixture("whois", "iana-google.txt"));
    iana.answer("8.8.8.8", fixture("whois", "iana-ipv4-arin.txt"));
    iana.answer("193.0.6.139", fixture("whois", "iana-ipv4-ripe.txt"));
    iana.answer("193.0.6.140", fixture("whois", "iana-ipv4-arin.txt")); /* IANA sends it to ARIN, ARIN to RIPE */
    iana.answer("10.1.2.3", fixture("whois", "iana-private.txt"));
    iana.answer("2001:4860:4860::8888", fixture("whois", "iana-ipv6.txt"));
    iana.answer("example.com", fixture("whois", "iana-domain.txt"));
    iana.answer("loop.test", "refer: loop-a.example\n");
    iana.answer("chain.test", "refer: loop-c.example\n");
    iana.answer("gone.test", "refer: nobody.example\n");
    iana.answer("lost.test", "refer: lost.example\n");
    verisign.answer("google.com", fixture("whois", "verisign-google.txt"));
    verisign.answer("www.google.com", fixture("whois", "verisign-nomatch.txt"));
    markmonitor.answer("google.com", fixture("whois", "markmonitor-google.txt"));
    arin.answer("n + 8.8.8.8", fixture("whois", "arin-8.8.8.8.txt"));
    arin.answer("n + 193.0.6.140", fixture("whois", "arin-ripe-space.txt"));
    arin.answer("n + 2001:4860:4860::8888", fixture("whois", "arin-ipv6.txt"));
    ripe.answer("193.0.6.139", fixture("whois", "ripe-193.0.6.139.txt"));
    ripe.answer("193.0.6.140", fixture("whois", "ripe-193.0.6.139.txt"));
    loop_a.fallback("Registrar WHOIS Server: loop-b.example\n");
    loop_b.fallback("Registrar WHOIS Server: loop-a.example\n");
    loop_c.fallback("Registrar WHOIS Server: loop-d.example\n");
    loop_d.fallback("Registrar WHOIS Server: loop-a.example\n"); /* c -> d -> a -> b: the fourth referral is refused */

    // a domain: IANA -> the registry -> the registrar
    WhoisResult r = run("google.com");
    CHECK(r.answers.size() == 3 && r.note.empty() && !r.is_ip);
    if (r.answers.size() == 3) {
        CHECK(r.answers[0].server == "whois.iana.org" && r.answers[1].server == "whois.verisign-grs.com" &&
              r.answers[2].server == "whois.markmonitor.com");
        CHECK(r.answers[2].ok && !r.answers[2].lines.empty());
    }
    CHECK(r.summary.registrar == "MarkMonitor, Inc." && r.summary.created == "1997-09-15T07:00:00+0000");
    CHECK(r.summary.name_servers.size() == 4 && r.summary.status.size() == 6);
    CHECK(r.summary.netname.empty()); /* IANA's "organisation: VeriSign ..." is not the domain's */
    // www.google.com: the registry has no such domain, the parent is asked
    r = run("www.google.com");
    CHECK(r.answers.size() == 4 && r.summary.registrar == "MarkMonitor, Inc.");
    if (r.answers.size() == 4) CHECK(r.answers[1].query == "www.google.com" && r.answers[2].query == "google.com");
    // an address: ARIN gets "n + <address>"
    r = run("8.8.8.8");
    CHECK(r.is_ip && r.answers.size() == 2 && r.summary.netname == "GOGL" && r.summary.cidr == "8.8.8.0/24");
    CHECK(r.summary.abuse == "network-abuse@google.com" && r.summary.organisation == "Google LLC");
    const std::vector<std::string> aq = arin.queries();
    CHECK(!aq.empty() && aq.back() == "n + 8.8.8.8");
    r = run("2001:4860:4860:0::8888");
    CHECK(r.answers.size() == 2 && r.summary.netname == "GOOGLE-IPV6");
    // RIPE space: straight from IANA, and through ARIN's ReferralServer
    r = run("193.0.6.139");
    CHECK(r.answers.size() == 2 && r.summary.netname == "RIPE-NCC" && r.summary.abuse == "abuse@ripe.net");
    CHECK(ripe.queries().back() == "193.0.6.139");
    r = run("193.0.6.140");
    CHECK(r.answers.size() == 3 && r.summary.netname == "RIPE-NCC" && r.summary.country == "NL");
    // IANA's own answer is the summary when nothing else answered
    r = run("10.1.2.3");
    CHECK(r.answers.size() == 1 && r.summary.organisation == "IANA - Private Use");
    r = run("example.com");
    CHECK(r.answers.size() == 1 && r.summary.created == "1992-01-01");
    // a referral loop stops; the referral limit holds
    r = run("loop.test");
    CHECK(r.answers.size() == 3 && r.note.find("referral loop stopped") == 0);
    r = run("chain.test");
    CHECK(r.answers.size() == 4 && r.note == "referral limit reached (loop-b.example not asked)");
    // a server that does not exist, or refuses
    r = run("lost.test");
    CHECK(r.answers.size() == 2 && !r.answers[1].ok && r.answers[1].error.find("cannot find lost.example") == 0);
    r = run("gone.test");
    CHECK(r.answers.size() == 2 && !r.answers[1].ok && r.answers[1].error == "connection refused");
    // an oversized answer: 64 kB kept, 400 lines in all
    iana.mode = static_cast<int>(FakeWhois::Mode::Endless);
    r = run("big.test");
    CHECK(r.answers.size() == 1 && r.answers[0].ok && r.answers[0].truncated && r.answers[0].bytes == kWhoisMaxAnswerBytes);
    CHECK(r.line_count() <= kWhoisMaxLines);
    for (const std::string &l : r.answers.empty() ? std::vector<std::string>() : r.answers[0].lines)
        CHECK(l.size() <= kWhoisMaxLineBytes);
    // garbage bytes: cleaned text, no summary, no crash
    iana.mode = static_cast<int>(FakeWhois::Mode::Answer);
    std::string junk;
    std::uint32_t x = 12345;
    for (int i = 0; i < 20000; ++i) {
        x = x * 1103515245u + 12345u;
        junk += static_cast<char>((x >> 16) & 0xFF);
    }
    iana.answer("junk.test", junk);
    r = run("junk.test");
    CHECK(r.answers.size() == 1 && r.answers[0].ok && r.summary.empty());
    bool clean = true;
    for (const WhoisAnswer &a : r.answers)
        for (const std::string &l : a.lines)
            if (!valid_utf8_printable(l)) clean = false;
    CHECK(clean);
    // more lines than the budget: the result keeps 400
    std::string many;
    for (int i = 0; i < 1000; ++i) many += "remarks: line " + std::to_string(i) + "\n";
    iana.answer("many.test", many);
    r = run("many.test");
    CHECK(r.line_count() == kWhoisMaxLines && r.lines_truncated);
    // a silent server: the time limit
    iana.mode = static_cast<int>(FakeWhois::Mode::Silent);
    {
        WhoisRequest req = base;
        req.query = "slow.test";
        req.total_ms = 800;
        const std::int64_t t0 = now_ms();
        r = run_whois(req, CancelToken());
        const std::int64_t took = now_ms() - t0;
        CHECK(r.answers.size() == 1 && !r.answers[0].ok && r.answers[0].error == "timed out");
        CHECK(took >= 750 && took < 1500);
    }
    // the session: Done; a bad query fails at once; a stop ends it within a slice
    {
        iana.mode = static_cast<int>(FakeWhois::Mode::Answer);
        WhoisSession s;
        WhoisRequest req = base;
        req.query = "8.8.8.8";
        CHECK(s.start(req));
        CHECK(s.wait(3000));
        auto snap = s.latest();
        CHECK(snap->state == WhoisSnapshot::State::Done && snap->result.summary.netname == "GOGL" && snap->asking.empty());
        req.query = "-h";
        CHECK(!s.start(req) && s.latest()->state == WhoisSnapshot::State::Failed);
        iana.mode = static_cast<int>(FakeWhois::Mode::Silent);
        req.query = "slow.test";
        CHECK(s.start(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(s.latest()->state == WhoisSnapshot::State::Running && s.latest()->asking == "whois.iana.org");
        const std::int64_t t0 = now_ms();
        s.stop();
        CHECK(s.wait(1000) && now_ms() - t0 < 300);
        CHECK(s.latest()->state == WhoisSnapshot::State::Cancelled);
        iana.mode = static_cast<int>(FakeWhois::Mode::Answer);
    }
    remove_dir(dir);
}

} // namespace

void test_whois()
{
    test_whois_parsers();
    test_whois_client();
}
