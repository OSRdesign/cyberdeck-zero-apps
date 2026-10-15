/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the public address. Cloudflare's trace (recorded on the deck, tests/fixtures/publicip; the
 * board's public address replaced by 203.0.113.7), the failed IPv6 request (no IPv6 on that network), the address
 * classes, the NAT words, the flow and the session on a fake runner.
 */
#include "check.hpp"

#include "publicip.hpp"

#include <map>

using namespace nt;

namespace {

CommandResult exited(int code, const std::string &output)
{
    CommandResult r;
    r.started = true;
    r.exit_code = code;
    r.output = output;
    return r;
}

class ByFamily : public CommandRunner {
public:
    CommandResult v4, v6;
    std::vector<std::vector<std::string>> asked;
    CommandResult run(const CommandSpec &spec, const CancelToken &) override
    {
        asked.push_back(spec.argv);
        return spec.argv.size() > 1 && spec.argv[1] == "-6" ? v6 : v4;
    }
};

} // namespace

void test_publicip()
{
    CfTrace t;
    CHECK(parse_cf_trace(fixture("publicip", "trace4.txt"), t));
    CHECK(t.ok && t.ip == "203.0.113.7" && t.loc == "FR" && t.colo == "CDG" && t.http == "http/2");
    CHECK(t.tls == "TLSv1.3" && t.warp == "off" && t.sni == "plaintext");
    CHECK(parse_cf_trace("ip=2a01:cb00:1234::1\nloc=DE\ncolo=FRA\n", t) && t.ip == "2a01:cb00:1234::1" && t.loc == "DE");
    CHECK(!parse_cf_trace("", t) && !t.ok && t.ip.empty());
    CHECK(!parse_cf_trace("ip=not-an-ip\nloc=FR\n", t) && t.ip.empty());
    CHECK(!parse_cf_trace("<html>captive portal</html>\n", t));
    CHECK(parse_cf_trace("noise\n=x\nip= 9.9.9.9 \ncolo=" + std::string(500, 'A') + "\n", t) && t.ip == "9.9.9.9" &&
          t.colo.size() == 64);
    CHECK(publicip_argv(4) == std::vector<std::string>({"curl", "-4", "-sS", "-m", "8", "--proto", "=https",
                                                         "https://www.cloudflare.com/cdn-cgi/trace"}));
    CHECK(publicip_argv(6)[1] == "-6");

    // the classes
    struct Row {
        const char *ip;
        IpClass cls;
    };
    const Row rows[] = {
        {"8.8.8.8", IpClass::Public},          {"1.0.0.1", IpClass::Public},  {"10.1.2.3", IpClass::Private},
        {"172.16.0.1", IpClass::Private},      {"172.31.255.255", IpClass::Private}, {"172.32.0.1", IpClass::Public},
        {"172.15.255.255", IpClass::Public},   {"192.168.68.121", IpClass::Private}, {"100.64.0.1", IpClass::Cgnat},
        {"100.127.255.254", IpClass::Cgnat},   {"100.63.255.255", IpClass::Public},  {"100.128.0.1", IpClass::Public},
        {"127.0.0.1", IpClass::Loopback},      {"169.254.10.20", IpClass::LinkLocal}, {"0.0.0.0", IpClass::Reserved},
        {"224.0.0.251", IpClass::Reserved},    {"255.255.255.255", IpClass::Reserved}, {"192.0.2.1", IpClass::Reserved},
        {"203.0.113.7", IpClass::Reserved},    {"198.18.0.1", IpClass::Reserved},    {"2a01:cb00::1", IpClass::Public},
        {"2001:4860:4860::8888", IpClass::Public}, {"::1", IpClass::Loopback},       {"fe80::1", IpClass::LinkLocal},
        {"fd12:3456::1", IpClass::Private},    {"2001:db8::1", IpClass::Reserved},   {"ff02::1", IpClass::Reserved},
        {"::", IpClass::Reserved},             {"::ffff:100.64.1.1", IpClass::Cgnat}, {"::ffff:8.8.8.8", IpClass::Public},
        {"192.168.68.121/22", IpClass::Private}, {"example.com", IpClass::Invalid},  {"", IpClass::Invalid},
    };
    for (const Row &r : rows) {
        const IpClass got = ip_class(r.ip);
        CHECK(got == r.cls);
        if (got != r.cls) std::fprintf(stderr, "  ip_class(%s) = %s\n", r.ip, ip_class_name(got));
    }
    CHECK_EQ(std::string(ip_class_name(IpClass::Cgnat)), std::string("carrier-grade NAT"));
    CHECK(nat_kind("192.168.68.121/22", "1.0.0.1") == NatKind::Nat);
    CHECK(nat_kind("100.72.3.4", "1.0.0.1") == NatKind::Cgnat);
    CHECK(nat_kind("1.0.0.1", "1.0.0.1") == NatKind::Direct);
    CHECK(nat_kind("", "1.0.0.1") == NatKind::Unknown && nat_kind("10.0.0.2", "") == NatKind::Unknown);

    // one request's words
    PublicIpFamily f = parse_publicip(exited(0, fixture("publicip", "trace4.txt")), 4);
    CHECK(f.tried && f.ok && f.trace.ip == "203.0.113.7" && f.cls == IpClass::Reserved && f.error.empty());
    f = parse_publicip(exited(7, fixture("publicip", "trace6-fail.txt")), 6);
    CHECK(f.tried && !f.ok && f.error == "no IPv6" && f.trace.ip.empty());
    f = parse_publicip(exited(7, ""), 4);
    CHECK(!f.ok && f.error == "no connection to Cloudflare");
    f = parse_publicip(exited(6, ""), 4);
    CHECK(f.error.find("cannot be found") != std::string::npos);
    f = parse_publicip(exited(127, ""), 4);
    CHECK(f.error.find("curl is not installed") == 0);
    f = parse_publicip(exited(0, "ip=8.8.8.8\n"), 6); /* an IPv4 answer to the IPv6 request: not taken */
    CHECK(!f.ok);
    f = parse_publicip(exited(0, "<html>sign in</html>"), 4);
    CHECK(!f.ok && f.error == "an answer that is not Cloudflare's trace");
    CommandResult late;
    late.started = true;
    late.timed_out = true;
    CHECK(parse_publicip(late, 4).error == "timed out");

    // the flow and the session: IPv4 then IPv6
    {
        auto run = std::make_shared<ByFamily>();
        run->v4 = exited(0, "fl=1\nip=1.0.0.1\nloc=FR\ncolo=CDG\n");
        run->v6 = exited(7, fixture("publicip", "trace6-fail.txt"));
        PublicIpSession s(run);
        CHECK(s.start());
        CHECK(s.wait(2000));
        const auto snap = s.latest();
        CHECK(snap->state == PublicIpSnapshot::State::Done && snap->family == 0);
        CHECK(snap->result.v4.ok && snap->result.v4.cls == IpClass::Public && snap->result.v4.trace.colo == "CDG");
        CHECK(!snap->result.v6.ok && snap->result.v6.error == "no IPv6");
        CHECK(run->asked.size() == 2 && run->asked[0][1] == "-4" && run->asked[1][1] == "-6");
    }
}
