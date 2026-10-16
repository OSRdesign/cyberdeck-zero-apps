/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the device-name packet builders and parsers (mDNS reverse PTR over UDP 5353 with the QU bit,
 * NetBIOS NBSTAT over UDP 137) on synthetic packets, and the name priority (mDNS before NetBIOS before reverse DNS).
 */
#include "check.hpp"
#include "fakes.hpp"

#include "dns.hpp"
#include "lannames.hpp"
#include "netaddr.hpp"

using namespace nt;

namespace {

Address a_of(const char *text)
{
    Address a;
    parse_ip(text, a);
    return a;
}

/* a minimal NBSTAT node-status answer carrying one unique workstation name */
std::vector<std::uint8_t> nbstat_answer(const std::string &name, int suffix, bool group)
{
    std::vector<std::uint8_t> b;
    b.insert(b.end(), {0x12, 0x34});         /* id */
    b.insert(b.end(), {0x84, 0x00});         /* flags: response */
    b.insert(b.end(), {0x00, 0x00});         /* qdcount = 0 */
    b.insert(b.end(), {0x00, 0x01});         /* ancount = 1 */
    b.insert(b.end(), {0x00, 0x00, 0x00, 0x00});
    b.push_back(0x00);                       /* answer name: root label */
    b.insert(b.end(), {0x00, 0x21});         /* type NBSTAT */
    b.insert(b.end(), {0x00, 0x01});         /* class IN */
    b.insert(b.end(), {0x00, 0x00, 0x00, 0x00}); /* ttl */
    b.insert(b.end(), {0x00, 0x00});         /* rdlength (unused by the parser) */
    b.push_back(0x01);                       /* one node name */
    std::string padded = name;
    padded.resize(15, ' ');
    for (char c : padded) b.push_back(static_cast<std::uint8_t>(c));
    b.push_back(static_cast<std::uint8_t>(suffix));
    const std::uint16_t flags = group ? 0x8000 : 0x0400;
    b.push_back(static_cast<std::uint8_t>(flags >> 8));
    b.push_back(static_cast<std::uint8_t>(flags & 0xFF));
    return b;
}

} // namespace

void test_lannames()
{
    // trim_local_suffix
    CHECK_EQ(trim_local_suffix("macbook.local"), std::string("macbook"));
    CHECK_EQ(trim_local_suffix("printer.lan"), std::string("printer"));
    CHECK_EQ(trim_local_suffix("nas.home"), std::string("nas"));
    CHECK_EQ(trim_local_suffix("pc.fritz.box"), std::string("pc"));
    CHECK_EQ(trim_local_suffix("plain"), std::string("plain"));
    CHECK_EQ(trim_local_suffix("MYHOST.LOCAL"), std::string("MYHOST")); /* case-insensitive suffix */

    // the name priority
    {
        const NamePick p = best_name("macbook", "WINPC", "box.local");
        CHECK(p.name == "macbook" && p.source == NameSource::Mdns);
    }
    {
        const NamePick p = best_name("", "WINPC", "box.local");
        CHECK(p.name == "WINPC" && p.source == NameSource::Netbios);
    }
    {
        const NamePick p = best_name("", "", "router.local");
        CHECK(p.name == "router" && p.source == NameSource::ReverseDns);
    }
    {
        const NamePick p = best_name("", "", "");
        CHECK(p.name.empty() && p.source == NameSource::None);
    }
    CHECK_EQ(std::string(name_source_label(NameSource::Mdns)), std::string("mDNS"));
    CHECK_EQ(std::string(name_source_label(NameSource::Netbios)), std::string("NetBIOS"));

    // the mDNS query: a PTR for the reverse name, with the QU bit on the question class
    {
        const Address a = a_of("192.168.1.5");
        const std::vector<std::uint8_t> q = build_mdns_reverse_query(a, 0x1234);
        CHECK(!q.empty());
        dns::Message m;
        CHECK(dns::parse_message(q.data(), q.size(), m));
        CHECK(m.questions.size() == 1 && m.questions[0].type == dns::kPTR);
        CHECK_EQ(m.questions[0].name, reverse_name(a));
        CHECK((m.questions[0].cls & 0x8000) != 0); /* the QU (unicast-response) bit */
        CHECK(build_mdns_reverse_query(Address(), 1).empty()); /* a bad address: empty */

        // parse an mDNS answer built from that query
        const std::vector<std::uint8_t> resp =
            fake::make_response(q, dns::kNoError, {{reverse_name(a), dns::kPTR, 120, fake::rd_name("macbook.local")}});
        CHECK_EQ(parse_mdns_name(resp.data(), resp.size()), std::string("macbook"));
        // no PTR answer: ""
        const std::vector<std::uint8_t> empty = fake::make_response(q, dns::kNxDomain, {});
        CHECK(parse_mdns_name(empty.data(), empty.size()).empty());
    }

    // the NBSTAT query and answer
    {
        const std::vector<std::uint8_t> q = build_nbstat_query(0x1234);
        CHECK(q.size() == 50 && q[0] == 0x12 && q[1] == 0x34);
        CHECK(q[12] == 0x20 && q[13] == 'C' && q[14] == 'K'); /* the encoded wildcard "*" */
        CHECK(q[47] == 0x21);                                 /* type NBSTAT */

        const std::vector<std::uint8_t> ans = nbstat_answer("DESKTOP-PC", 0x00, false);
        CHECK_EQ(parse_nbstat_name(ans.data(), ans.size()), std::string("DESKTOP-PC"));
        // a group name (flags top bit set) is not a workstation name
        const std::vector<std::uint8_t> grp = nbstat_answer("WORKGROUP", 0x00, true);
        CHECK(parse_nbstat_name(grp.data(), grp.size()).empty());
        // a non-zero suffix is not the workstation name either
        const std::vector<std::uint8_t> svc = nbstat_answer("FILESERVER", 0x20, false);
        CHECK(parse_nbstat_name(svc.data(), svc.size()).empty());
        // a short / malformed packet
        CHECK(parse_nbstat_name(ans.data(), 8).empty());
        CHECK(parse_mdns_name(ans.data(), 4).empty());
    }
}
