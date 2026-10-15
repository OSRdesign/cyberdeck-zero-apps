/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the DNS wire format (build / parse round trips, every record type, compression), crafted bad
 * packets and a mutation fuzz (nothing crashes, every loop ends), and the client against fake servers on loopback
 * (UDP, the TCP fallback, retries, a silent server, a closed port, FORMERR without EDNS, compare, resolve_host, the
 * session and its cancel).
 */
#include "check.hpp"
#include "fakes.hpp"

#include "dns.hpp"
#include "dns_client.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

using namespace nt;
using fake::Bytes;

namespace {

Bytes query_for(const std::string &name, std::uint16_t type, std::uint16_t id = 0x1234, bool edns = true)
{
    dns::QueryOptions opt;
    opt.edns = edns;
    Bytes q;
    dns::build_query(name, type, id, opt, q);
    return q;
}

bool parse(const Bytes &b, dns::Message &m, std::string *why = nullptr)
{
    return dns::parse_message(b.data(), b.size(), m, why);
}

} // namespace

void test_dns_wire()
{
    // the query itself
    Bytes q = query_for("www.Example.com", dns::kAAAA, 0xBEEF);
    CHECK(q.size() == 12 + 17 + 4 + 11);
    CHECK(q[0] == 0xBE && q[1] == 0xEF && q[2] == 0x01 && q[3] == 0x00); /* RD */
    CHECK(q[5] == 1 && q[7] == 0 && q[11] == 1);                          /* one question, one OPT */
    CHECK(q[12] == 3 && q[13] == 'w' && q[16] == 7);
    dns::Message m;
    CHECK(parse(q, m));
    CHECK(m.header.id == 0xBEEF && !m.header.qr && m.header.rd && m.questions.size() == 1);
    CHECK(m.questions[0].name == "www.Example.com" && m.questions[0].type == dns::kAAAA && m.questions[0].cls == 1);
    CHECK(m.edns && m.udp_size == 1232 && m.additional.empty());
    CHECK_EQ(query_for("example.com.", dns::kA, 1, false).size(), static_cast<std::size_t>(12 + 13 + 4));
    std::string why;
    Bytes bad;
    CHECK(!dns::build_query("a..b", dns::kA, 1, dns::QueryOptions(), bad, &why) && !why.empty());
    CHECK(!dns::build_query("", dns::kA, 1, dns::QueryOptions(), bad));
    CHECK(!dns::build_query(std::string(64, 'x') + ".com", dns::kA, 1, dns::QueryOptions(), bad));

    // every record type, a CNAME chain, an authority SOA
    q = query_for("www.example.com", dns::kA);
    std::vector<fake::Record> ans = {
        {"www.example.com", dns::kCNAME, 3600, fake::rd_name("edge.example.net")},
        {"edge.example.net", dns::kCNAME, 60, fake::rd_name("e1.cdn.example.org")},
        {"e1.cdn.example.org", dns::kA, 20, fake::rd_a("93.184.215.14")},
        {"e1.cdn.example.org", dns::kA, 20, fake::rd_a("93.184.215.15")},
    };
    Bytes resp = fake::make_response(q, 0, ans);
    CHECK(parse(resp, m));
    CHECK(m.header.qr && m.header.ra && m.header.rcode == 0 && m.answers.size() == 4);
    CHECK(m.answers[0].name == "www.example.com" && m.answers[0].type == dns::kCNAME && m.answers[0].ttl == 3600);
    CHECK_EQ(m.answers[0].data, std::string("edge.example.net"));
    CHECK_EQ(m.answers[2].data, std::string("93.184.215.14"));
    CHECK(dns::response_matches(m, 0x1234, "WWW.example.com.", dns::kA));
    CHECK(!dns::response_matches(m, 0x1235, "www.example.com", dns::kA));
    CHECK(!dns::response_matches(m, 0x1234, "www.example.com", dns::kAAAA));
    const std::vector<std::string> chain = dns::cname_chain(m, "www.example.com");
    CHECK(chain.size() == 3 && chain[2] == "e1.cdn.example.org");

    q = query_for("example.com", dns::kMX);
    resp = fake::make_response(q, 0,
                               {{"example.com", dns::kMX, 300, fake::rd_mx(10, "mail.example.com")},
                                {"example.com", dns::kAAAA, 300, fake::rd_aaaa("2606:2800:21f:cb07:6820:80da:af6b:8b2c")},
                                {"example.com", dns::kTXT, 300, fake::rd_txt({"v=spf1 -all", "say \"hi\"\\"})},
                                {"example.com", dns::kNS, 300, fake::rd_name("a.iana-servers.net")},
                                {"14.215.184.93.in-addr.arpa", dns::kPTR, 300, fake::rd_name("example.com")},
                                {"example.com", 99, 300, Bytes{0xDE, 0xAD}}});
    CHECK(parse(resp, m));
    CHECK_EQ(m.answers.size(), static_cast<std::size_t>(6));
    CHECK_EQ(m.answers[0].data, std::string("10 mail.example.com"));
    CHECK_EQ(m.answers[1].data, std::string("2606:2800:21f:cb07:6820:80da:af6b:8b2c"));
    CHECK_EQ(m.answers[2].data, std::string("\"v=spf1 -all\" \"say \\\"hi\\\"\\\\\""));
    CHECK_EQ(m.answers[3].data, std::string("a.iana-servers.net"));
    CHECK_EQ(m.answers[4].data, std::string("example.com"));
    CHECK_EQ(m.answers[5].data, std::string("\\# 2 dead"));

    // SOA in the authority section of an NXDOMAIN, and SRV
    {
        Bytes soa = fake::wire_name("ns.icann.org");
        const Bytes rname = fake::wire_name("noc.dns.icann.org");
        soa.insert(soa.end(), rname.begin(), rname.end());
        for (std::uint32_t v : {2026101501u, 7200u, 3600u, 1209600u, 3600u}) {
            soa.push_back(static_cast<std::uint8_t>(v >> 24));
            soa.push_back(static_cast<std::uint8_t>(v >> 16));
            soa.push_back(static_cast<std::uint8_t>(v >> 8));
            soa.push_back(static_cast<std::uint8_t>(v));
        }
        q = query_for("nope.example.com", dns::kA);
        resp = fake::make_response(q, dns::kNxDomain, {}, false, 0, {{"example.com", dns::kSOA, 3600, soa}});
        CHECK(parse(resp, m));
        CHECK(m.header.rcode == dns::kNxDomain && m.answers.empty() && m.authority.size() == 1);
        CHECK_EQ(m.authority[0].data, std::string("ns.icann.org noc.dns.icann.org 2026101501 7200 3600 1209600 3600"));
        Bytes srv = {0, 10, 0, 5, 0x13, 0xC4};
        const Bytes target = fake::wire_name("sip.example.com");
        srv.insert(srv.end(), target.begin(), target.end());
        q = query_for("_sip._tcp.example.com", dns::kSRV);
        resp = fake::make_response(q, 0, {{"_sip._tcp.example.com", dns::kSRV, 60, srv}});
        CHECK(parse(resp, m) && m.answers.size() == 1 && m.answers[0].data == "10 5 5060 sip.example.com");
    }

    // a compressed name inside record data (MX exchange -> the question name), an extended rcode from OPT
    {
        q = query_for("example.com", dns::kMX);
        Bytes r = fake::make_response(q, 0, {});
        r[7] = 1;                                   /* ancount 1 */
        const Bytes rr = {0xC0, 0x0C, 0x00, 0x0F, 0x00, 0x01, 0, 0, 0, 60, 0x00, 0x09,
                          0x00, 0x05, 0x04, 'm', 'a', 'i', 'l', 0xC0, 0x0C};
        r.insert(r.end(), rr.begin(), rr.end());
        CHECK(parse(r, m) && m.answers.size() == 1 && m.answers[0].data == "5 mail.example.com");
        r[11] = 1;                                  /* arcount 1: an OPT with extended rcode 1 (BADVERS = 16) */
        const Bytes opt = {0x00, 0x00, 0x29, 0x04, 0xD0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
        r.insert(r.end(), opt.begin(), opt.end());
        CHECK(parse(r, m) && m.edns && m.udp_size == 1232 && m.header.rcode == 16);
        CHECK_EQ(dns::rcode_name(16), std::string("BADVERS"));
    }

    // names: case, the root, escapes
    CHECK(dns::names_equal("Example.COM.", "example.com") && !dns::names_equal("a.com", "b.com"));
    {
        const Bytes odd = {0x03, 'a', '.', 'b', 0x02, 0x01, ' ', 0x00};
        std::string name;
        std::size_t next = 0;
        CHECK(dns::read_name(odd.data(), odd.size(), 0, name, &next) && next == odd.size());
        CHECK_EQ(name, std::string("a\\046b.\\001\\032"));
        const Bytes root = {0x00};
        CHECK(dns::read_name(root.data(), root.size(), 0, name, &next) && name == "." && next == 1);
    }

    // words and names of types and codes
    CHECK_EQ(dns::rcode_name(dns::kNxDomain), std::string("NXDOMAIN"));
    CHECK_EQ(dns::rcode_words(dns::kNxDomain), std::string("no such name"));
    CHECK_EQ(dns::rcode_words(dns::kServFail), std::string("server failure"));
    CHECK_EQ(dns::rcode_words(dns::kRefused), std::string("refused"));
    CHECK_EQ(dns::rcode_name(11), std::string("RCODE 11"));
    std::uint16_t t = 0;
    CHECK(dns::type_from_name("aaaa", t) && t == dns::kAAAA);
    CHECK(dns::type_from_name(" Mx ", t) && t == dns::kMX);
    CHECK(!dns::type_from_name("XYZ", t));
    CHECK_EQ(dns::type_name(99), std::string("TYPE99"));
    CHECK_EQ(dns::lookup_types().size(), static_cast<std::size_t>(7));

    // the CNAME chain stops on a loop
    {
        dns::Message loop;
        dns::Record a, b;
        a.type = b.type = dns::kCNAME;
        a.name = "x.test";
        a.data = "y.test";
        b.name = "y.test";
        b.data = "x.test";
        loop.answers = {a, b};
        CHECK_EQ(dns::cname_chain(loop, "x.test").size(), static_cast<std::size_t>(2));
    }

    // what the user types
    std::uint16_t type = dns::kA;
    std::string qname;
    CHECK(make_question(" example.com. ", type, qname) && qname == "example.com" && type == dns::kA);
    CHECK(make_question("1.1.1.1", type, qname) && qname == "1.1.1.1.in-addr.arpa" && type == dns::kPTR);
    type = dns::kMX;
    CHECK(!make_question("bad name", type, qname, &why) && type == dns::kMX);
    CHECK_EQ(pick_resolver("", {"192.168.1.1", "192.168.68.1"}), std::string("192.168.1.1"));
    CHECK_EQ(pick_resolver("system", {"not-ip", "9.9.9.9"}), std::string("9.9.9.9"));
    CHECK_EQ(pick_resolver("1.0.0.1", {"192.168.1.1"}), std::string("1.0.0.1"));
    CHECK_EQ(pick_resolver("dns.google", {"192.168.1.1"}), std::string(""));
    CHECK_EQ(pick_resolver("", {}), std::string(""));
    CHECK(resolver_presets().size() == 4 && std::string(resolver_presets()[1].address) == "1.1.1.1");
}

void test_dns_fuzz()
{
    dns::Message m;
    std::string why;
    std::string name;
    std::size_t next = 0;
    // crafted bad packets
    CHECK(!dns::parse_message(nullptr, 0, m, &why));
    const Bytes eleven(11, 0);
    CHECK(!dns::parse_message(eleven.data(), eleven.size(), m, &why) && why == "shorter than a DNS header");
    Bytes h = {0, 1, 0x81, 0x80, 0, 1, 0, 0, 0, 0, 0, 0}; /* one question, none there */
    CHECK(!parse(h, m, &why));
    Bytes self = h;
    self.insert(self.end(), {0xC0, 0x0C, 0, 1, 0, 1}); /* a pointer to itself */
    CHECK(!parse(self, m, &why) && why.find("point back") != std::string::npos);
    Bytes fwd = h;
    fwd.insert(fwd.end(), {0xC0, 0x12, 0, 1, 0, 1, 0x01, 'a', 0x00}); /* a forward pointer */
    CHECK(!parse(fwd, m, &why));
    Bytes cut = h;
    cut.insert(cut.end(), {0x3F, 'a', 'b'}); /* a label of 63 bytes, 2 there */
    CHECK(!parse(cut, m, &why) && why.find("past the end") != std::string::npos);
    Bytes ext = h;
    ext.insert(ext.end(), {0x41, 'a', 0x00, 0, 1, 0, 1}); /* label type 01 (extended) */
    CHECK(!parse(ext, m, &why) && why == "an unknown label type");
    Bytes ptr = h;
    ptr.push_back(0xC0); /* a pointer cut in half */
    CHECK(!parse(ptr, m, &why));
    {
        Bytes chain = {0x01, 'a', 0x00};
        for (int i = 0; i < 40; ++i) {
            const std::size_t prev = i == 0 ? 0 : chain.size() - 2;
            chain.push_back(static_cast<std::uint8_t>(0xC0 | (prev >> 8)));
            chain.push_back(static_cast<std::uint8_t>(prev & 0xFF));
        }
        CHECK(dns::read_name(chain.data(), chain.size(), 3 + 2 * 19, name, &next) && name == "a"); /* 20 hops */
        CHECK(!dns::read_name(chain.data(), chain.size(), chain.size() - 2, name, &next, &why) &&
              why == "too many compression pointers"); /* 40 hops */
    }
    {
        Bytes longname;
        for (int i = 0; i < 4; ++i) {
            longname.push_back(63);
            longname.insert(longname.end(), 63, 'x');
        }
        longname.push_back(0); /* 257 bytes on the wire */
        CHECK(!dns::read_name(longname.data(), longname.size(), 0, name, &next, &why) &&
              why == "a name longer than 255 bytes");
    }
    // counts that cannot fit, a record that runs past the end, malformed data kept as text
    Bytes q = query_for("example.com", dns::kA);
    Bytes good = fake::make_response(q, 0, {{"example.com", dns::kA, 60, fake::rd_a("1.2.3.4")}});
    Bytes many = good;
    many[6] = 0xFF;
    many[7] = 0xFF;
    CHECK(!parse(many, m, &why) && why == "too many records");
    many[6] = 0x00; /* 255: under the cap, but 255 records cannot fit in these bytes */
    CHECK(!parse(many, m, &why) && why == "the record counts do not fit the packet");
    Bytes shortrd = good;
    shortrd.pop_back();
    CHECK(!parse(shortrd, m, &why));
    Bytes bigrd = good;
    bigrd[bigrd.size() - 5] = 0x40; /* rdlength 0x0040, 4 bytes there */
    CHECK(!parse(bigrd, m, &why) && why == "a record's data runs past the end");
    Bytes five = fake::make_response(q, 0, {{"example.com", dns::kA, 60, Bytes{1, 2, 3, 4, 5}}});
    CHECK(parse(five, m) && m.answers.size() == 1 && m.answers[0].data == "(malformed, 5 bytes)");
    Bytes txt = fake::make_response(q, 0, {{"example.com", dns::kTXT, 60, Bytes{10, 'a', 'b'}}});
    CHECK(parse(txt, m) && m.answers[0].data == "(malformed, 3 bytes)");
    Bytes mx = fake::make_response(q, 0, {{"example.com", dns::kMX, 60, Bytes{0, 1, 5, 'a'}}});
    CHECK(parse(mx, m) && m.answers[0].data == "(malformed, 4 bytes)");
    Bytes cname_out = fake::make_response(q, 0, {{"example.com", dns::kCNAME, 60, Bytes{0xC0, 0x0C, 0x00}}});
    CHECK(parse(cname_out, m) && m.answers[0].data == "(malformed, 3 bytes)"); /* the name ends before rdata does */
    Bytes ttl = fake::make_response(q, 0, {{"example.com", dns::kA, 0x80000001u, fake::rd_a("1.2.3.4")}});
    CHECK(parse(ttl, m) && m.answers[0].ttl == 0);

    // mutation fuzz: flips, cuts and random bytes; every parse returns, nothing crashes
    std::mt19937 rng(20261015);
    std::vector<Bytes> seeds;
    seeds.push_back(good);
    seeds.push_back(fake::make_response(query_for("www.example.com", dns::kA), 0,
                                        {{"www.example.com", dns::kCNAME, 60, fake::rd_name("e.example.net")},
                                         {"e.example.net", dns::kA, 60, fake::rd_a("5.6.7.8")}}));
    seeds.push_back(mx);
    seeds.push_back(txt);
    seeds.push_back(query_for("a.b.c.example.org", dns::kTXT));
    int parsed = 0, refused = 0;
    std::size_t longest = 0;
    for (int i = 0; i < 40000; ++i) {
        Bytes b = seeds[static_cast<std::size_t>(rng()) % seeds.size()];
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
            b.resize(rng() % (b.size() + 1));
        } else if (kind == 1 || kind == 2) {
            const int flips = 1 + static_cast<int>(rng() % 6);
            for (int f = 0; f < flips && !b.empty(); ++f) b[rng() % b.size()] = static_cast<std::uint8_t>(rng());
        } else {
            b.resize(rng() % 300);
            for (std::uint8_t &c : b) c = static_cast<std::uint8_t>(rng());
            if (b.size() > 12 && (rng() & 1)) {
                b[4] = 0;
                b[5] = 1; /* plausible counts */
                b[6] = 0;
                b[7] = static_cast<std::uint8_t>(rng() % 4);
            }
        }
        if (dns::parse_message(b.data(), b.size(), m, &why)) {
            ++parsed;
            for (const auto *list : {&m.answers, &m.authority, &m.additional})
                for (const dns::Record &r : *list) longest = std::max(longest, r.name.size() + r.data.size());
        } else {
            ++refused;
        }
        std::string n2;
        if (!b.empty()) dns::read_name(b.data(), b.size(), rng() % b.size(), n2, &next);
    }
    CHECK(parsed > 1000 && refused > 1000);
    CHECK(longest < 4096);
}

void test_dns_client()
{
    fake::Dns srv;
    if (!srv.ok()) {
        SKIP("cannot bind a DNS server on 127.0.0.1");
        return;
    }
    srv.add("example.com", dns::kA, {"example.com", dns::kA, 300, fake::rd_a("93.184.215.14")});
    srv.add("example.com", dns::kAAAA, {"example.com", dns::kAAAA, 300, fake::rd_aaaa("2606:2800::1")});
    srv.add("www.example.com", dns::kA, {"www.example.com", dns::kCNAME, 60, fake::rd_name("example.com")});
    srv.add("www.example.com", dns::kA, {"example.com", dns::kA, 300, fake::rd_a("93.184.215.14")});
    srv.add("v6only.test", dns::kAAAA, {"v6only.test", dns::kAAAA, 300, fake::rd_aaaa("2001:db8::7")});
    srv.add("1.0.0.127.in-addr.arpa", dns::kPTR, {"1.0.0.127.in-addr.arpa", dns::kPTR, 300, fake::rd_name("localhost")});
    CancelToken none;
    DnsQuery q;
    q.name = "www.example.com";
    q.server = "127.0.0.1";
    q.port = srv.port();
    q.timeout_ms = 400;
    q.tries = 2;

    // a normal answer with its chain
    DnsResult r = dns_query(q, none);
    CHECK(r.ok() && r.answered && r.rcode_name == "NOERROR" && r.rcode_words == "OK" && !r.via_tcp);
    CHECK(r.answers.size() == 2 && r.chain.size() == 2 && r.chain[1] == "example.com");
    CHECK(r.addresses(dns::kA).size() == 1 && r.server == "127.0.0.1" && r.elapsed_ms < 400 && r.tries_used == 1);
    // NXDOMAIN
    q.name = "nope.example.com";
    r = dns_query(q, none);
    CHECK(r.answered && !r.ok() && r.rcode == dns::kNxDomain && r.rcode_words == "no such name");
    // PTR
    q.name = "1.0.0.127.in-addr.arpa";
    q.type = dns::kPTR;
    r = dns_query(q, none);
    CHECK(r.ok() && r.answers.size() == 1 && r.answers[0].data == "localhost");
    // truncated over UDP: asked again over TCP
    srv.set_mode(fake::Dns::Mode::Truncate);
    q.name = "example.com";
    q.type = dns::kA;
    const int tcp_before = srv.tcp_queries.load();
    r = dns_query(q, none);
    CHECK(r.ok() && r.via_tcp && r.answers.size() == 1 && srv.tcp_queries.load() == tcp_before + 1);
    // an answer with the wrong id first: ignored, the right one taken
    srv.set_mode(fake::Dns::Mode::WrongIdFirst);
    r = dns_query(q, none);
    CHECK(r.ok() && r.answers.size() == 1);
    // FORMERR to EDNS: asked again without it
    srv.set_mode(fake::Dns::Mode::FormErrOnEdns);
    const int udp_before = srv.udp_queries.load();
    r = dns_query(q, none);
    CHECK(r.ok() && srv.udp_queries.load() == udp_before + 2);
    // SERVFAIL is an answer, not a transport error
    srv.set_mode(fake::Dns::Mode::ServFail);
    r = dns_query(q, none);
    CHECK(r.answered && r.rcode == dns::kServFail && r.rcode_words == "server failure");
    // silent: every try waits its timeout, then "timed out"
    srv.set_mode(fake::Dns::Mode::Silent);
    const int silent_before = srv.udp_queries.load();
    std::int64_t t0 = now_ms();
    r = dns_query(q, none);
    std::int64_t took = now_ms() - t0;
    CHECK(!r.answered && r.error == "timed out" && r.tries_used == 2);
    CHECK(took >= 750 && took < 1500);
    CHECK(srv.udp_queries.load() == silent_before + 2);
    // a cancel ends a silent wait within a slice
    {
        Worker w;
        DnsResult got;
        q.timeout_ms = 5000;
        w.start([&](const CancelToken &c) { got = dns_query(q, c); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        t0 = now_ms();
        w.cancel();
        CHECK(w.wait(1000));
        CHECK(now_ms() - t0 < 300);
        CHECK(!got.answered && got.error == "cancelled");
        q.timeout_ms = 400;
    }
    srv.set_mode(fake::Dns::Mode::Answer);
    // nothing on that port: refused at once (ICMP port unreachable on loopback)
    {
        DnsQuery c = q;
        c.port = fake::closed_port();
        t0 = now_ms();
        r = dns_query(c, none);
        CHECK(!r.answered && r.error.find("refused") == 0);
        CHECK(now_ms() - t0 < 300);
    }
    // a bad server
    {
        DnsQuery c = q;
        c.server = "not-an-ip";
        r = dns_query(c, none);
        CHECK(!r.answered && r.error == "not an IP address: not-an-ip");
        c.server = "";
        CHECK(dns_query(c, none).error == "no DNS server");
    }

    // compare: two servers (127.0.0.1 and a silent 127.0.0.2 on the same port), one result each, with timings
    {
        fake::Dns second("127.0.0.2", srv.port());
        if (!second.ok()) {
            SKIP("cannot bind a second DNS server on 127.0.0.2");
        } else {
            second.set_mode(fake::Dns::Mode::Silent);
            int seen = 0;
            q.tries = 1;
            const std::vector<DnsResult> all =
                dns_compare(q, {"127.0.0.1", "127.0.0.2"}, none, [&](const DnsResult &) { ++seen; });
            CHECK(all.size() == 2 && seen == 2);
            CHECK(all[0].ok() && all[0].server == "127.0.0.1");
            CHECK(!all[1].answered && all[1].server == "127.0.0.2" && all[1].elapsed_ms >= 380);
            q.tries = 2;
        }
    }

    // resolve_host: literal, /etc/hosts, A, AAAA only, NXDOMAIN, no server
    {
        const std::string dir = test_tmpdir("hosts");
        const std::string hosts = dir + "/hosts";
        std::ofstream(hosts) << "127.0.0.1\tlocalhost\n::1 localhost ip6-localhost\n192.168.68.121 zero7 # the deck\n";
        CHECK(hosts_lookup("127.0.0.1 localhost\n::1 localhost\n", "LOCALHOST").size() == 2);
        CHECK(hosts_lookup("::1 localhost\n127.0.0.1 localhost\n", "localhost")[0].family == AF_INET);
        CHECK(hosts_lookup("# 1.2.3.4 x\nbad line\n", "x").empty());
        ResolveOptions o;
        o.server = "127.0.0.1";
        o.port = srv.port();
        o.timeout_ms = 400;
        o.hosts_path = hosts;
        Resolved res = resolve_host("10.1.2.3", o, none);
        CHECK(res.ok && res.literal && res.address.text() == "10.1.2.3");
        res = resolve_host("zero7", o, none);
        CHECK(res.ok && res.from_hosts && res.address.text() == "192.168.68.121");
        res = resolve_host("example.com", o, none);
        CHECK(res.ok && !res.from_hosts && res.address.text() == "93.184.215.14" && res.server == "127.0.0.1");
        res = resolve_host("v6only.test", o, none);
        CHECK(res.ok && res.address.family == AF_INET6 && res.address.text() == "2001:db8::7");
        res = resolve_host("nope.example.com", o, none);
        CHECK(!res.ok && res.error == "nope.example.com does not exist (NXDOMAIN)");
        res = resolve_host("bad name", o, none);
        CHECK(!res.ok && !res.error.empty());
        o.server = "";
        res = resolve_host("example.com", o, none);
        CHECK(!res.ok && res.error.find("No DNS server") == 0);
        srv.set_mode(fake::Dns::Mode::Silent);
        o.server = "127.0.0.1";
        o.tries = 1;
        res = resolve_host("example.com", o, none);
        CHECK(!res.ok && res.error == "The DNS server 127.0.0.1 did not answer (timed out)");
        srv.set_mode(fake::Dns::Mode::Answer);
        remove_dir(dir);
    }

    // the session: lookup, compare, bad input, no server, cancel
    {
        DnsSession s;
        DnsRequest req;
        req.input = "www.example.com";
        req.type = dns::kA;
        req.servers = {"127.0.0.1"};
        req.port = srv.port();
        req.timeout_ms = 400;
        CHECK(s.start(req));
        CHECK(s.wait(3000));
        auto snap = s.latest();
        CHECK(snap->state == DnsSnapshot::State::Done && snap->results.size() == 1 && snap->results[0].ok());
        CHECK(!snap->compare() && snap->question == "www.example.com");
        req.input = "127.0.0.1"; /* an address: PTR whatever the type */
        CHECK(s.start(req) && s.wait(3000));
        snap = s.latest();
        CHECK(snap->type == dns::kPTR && snap->question == "1.0.0.127.in-addr.arpa" && snap->results[0].ok());
        req.input = "bad name";
        CHECK(!s.start(req));
        CHECK(s.latest()->state == DnsSnapshot::State::Failed && !s.latest()->error.empty());
        req.input = "example.com";
        req.servers = {"", "nonsense"};
        CHECK(!s.start(req));
        CHECK(s.latest()->error.find("No DNS server") == 0);
        srv.set_mode(fake::Dns::Mode::Silent);
        req.servers = {"127.0.0.1"};
        req.timeout_ms = 5000;
        CHECK(s.start(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(s.running() && s.latest()->state == DnsSnapshot::State::Running);
        s.stop();
        CHECK(s.wait(1000));
        CHECK(s.latest()->state == DnsSnapshot::State::Cancelled && s.latest()->results.empty());
        srv.set_mode(fake::Dns::Mode::Answer);
    }
}
