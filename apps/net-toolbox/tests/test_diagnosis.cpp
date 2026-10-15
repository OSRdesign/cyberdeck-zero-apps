/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: the diagnosis state machine on a fake network (the interface state from the deck's fixtures):
 * the all-good run, a failure at each step with its explanation, the fallbacks that keep a step green (a gateway that
 * ignores ping, a second resolver, ping not allowed, ping blocked on the internet), the notes, and a cancel.
 */
#include "check.hpp"

#include "diagnosis.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <thread>

using namespace nt;

namespace {

class FakeNet : public DiagProbe {
public:
    NetState st;
    bool ping_allowed = true;
    std::set<std::string> ping_answers = {"192.168.68.1", "1.1.1.1", "8.8.8.8"};
    std::map<std::string, std::set<int>> tcp_answers;      /* address -> ports that answer (open or refused) */
    std::set<std::string> dns_silent, dns_nxdomain;        /* resolvers */
    std::map<std::string, std::string> names = {{"example.com", "93.184.215.14"},
                                                {"connectivitycheck.gstatic.com", "142.250.179.99"}};
    bool http_answers = true;
    int http_status = 204;
    std::string http_location;
    bool block_ping = false;                               /* ping waits for the cancel */
    std::vector<std::string> asked_dns;
    int pings = 0;

    explicit FakeNet(const char *board = "deck")
    {
        const auto src = make_fixture_source(fixture_dir(board));
        st = read_net_state(*src);
    }
    NetState state() override { return st; }
    bool can_ping() override { return ping_allowed; }
    bool ping(const Address &ip, int, const CancelToken &cancel, double *rtt) override
    {
        ++pings;
        if (block_ping) {
            while (!cancel.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return false;
        }
        if (!ping_answers.count(ip.text())) return false;
        *rtt = ip.text() == "192.168.68.1" ? 3.0 : 14.0;
        return true;
    }
    DnsResult dns(const std::string &name, std::uint16_t type, const std::string &server, int,
                  const CancelToken &) override
    {
        asked_dns.push_back(server + " " + name);
        DnsResult r;
        r.name = name;
        r.type = type;
        r.server = server;
        if (dns_silent.count(server)) {
            r.error = "timed out";
            r.elapsed_ms = 2000;
            return r;
        }
        r.answered = true;
        r.elapsed_ms = 12;
        const auto it = names.find(name);
        if (dns_nxdomain.count(server) || it == names.end()) {
            r.rcode = dns::kNxDomain;
            r.rcode_name = "NXDOMAIN";
            r.rcode_words = "no such name";
            return r;
        }
        r.rcode = 0;
        r.rcode_name = "NOERROR";
        r.rcode_words = "OK";
        dns::Record rec;
        rec.name = name;
        rec.type = dns::kA;
        rec.data = it->second;
        r.answers.push_back(rec);
        return r;
    }
    PortResult tcp(const Address &ip, std::uint16_t port, int, const CancelToken &) override
    {
        PortResult r;
        r.port = port;
        const auto it = tcp_answers.find(ip.text());
        const bool answers = it != tcp_answers.end() && it->second.count(port);
        r.state = answers ? PortState::Open : PortState::Filtered;
        r.connect_ms = answers ? 21 : 0;
        return r;
    }
    HttpResult http(const Address &, const std::string &, const std::string &, int, const CancelToken &) override
    {
        HttpResult h;
        if (!http_answers) {
            h.error = "timed out";
            return h;
        }
        h.answered = true;
        h.status = http_status;
        h.location = http_location;
        return h;
    }
};

/* the ladder in one word: P pass, F fail, S skipped, R running, . pending */
std::string ladder(const DiagReport &r)
{
    std::string out;
    for (const DiagStep &s : r.steps) {
        switch (s.state) {
        case StepState::Pass: out += 'P'; break;
        case StepState::Fail: out += 'F'; break;
        case StepState::Skipped: out += 'S'; break;
        case StepState::Running: out += 'R'; break;
        default: out += '.'; break;
        }
    }
    return out;
}

DiagReport run(FakeNet &net, const DiagOptions &opt = DiagOptions(), int *published = nullptr)
{
    CancelToken none;
    return run_diagnosis(net, opt, none, [published](const DiagReport &) {
        if (published) ++*published;
    });
}

bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

void test_diagnosis()
{
    CHECK_EQ(std::string(step_title(Step::Portal)), std::string("Captive portal"));
    CHECK_EQ(std::string(step_state_name(StepState::Skipped)), std::string("skipped"));
    {
        const DiagReport idle;
        CHECK(idle.steps.size() == static_cast<std::size_t>(kStepCount) && ladder(idle) == "......");
    }

    // all good
    {
        FakeNet net;
        int published = 0;
        const DiagReport r = run(net, DiagOptions(), &published);
        CHECK_EQ(ladder(r), std::string("PPPPPP"));
        CHECK(r.state == DiagReport::State::Passed && r.failed == -1);
        CHECK_EQ(r.step(Step::Link).detail, std::string("wlan0"));
        CHECK_EQ(r.step(Step::Address).detail, std::string("192.168.68.121"));
        CHECK_EQ(r.step(Step::Gateway).detail, std::string("192.168.68.1, 3.0 ms"));
        CHECK_EQ(r.step(Step::Dns).detail, std::string("via 192.168.1.1, 12 ms")); /* resolv.conf's first */
        CHECK_EQ(r.step(Step::Internet).detail, std::string("1.1.1.1, 14 ms"));
        CHECK_EQ(r.step(Step::Portal).detail, std::string("none (204)"));
        CHECK_EQ(r.resolver, std::string("192.168.1.1"));
        CHECK(has(r.explanation, "Your connection works") && r.notes.empty());
        CHECK(published >= 13); /* the start, then each step running and passed */
        CHECK(net.asked_dns.size() == 2 && net.asked_dns[1] == "192.168.1.1 connectivitycheck.gstatic.com");
    }

    // Link: Wi-Fi up but not associated; no adapter at all; a cable unplugged
    {
        FakeNet net("deck-offline");
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("FSSSSS"));
        CHECK(r.state == DiagReport::State::Failed && r.failed == 0);
        CHECK_EQ(r.step(Step::Link).detail, std::string("wlan0 not connected"));
        CHECK(has(r.explanation, "Wi-Fi is not connected to a network"));
        CHECK(net.pings == 0 && net.asked_dns.empty());
    }
    {
        FakeNet net;
        std::vector<Iface> only_lo;
        for (const Iface &i : net.st.ifaces)
            if (i.loopback) only_lo.push_back(i);
        net.st.ifaces = only_lo;
        net.st.main_iface.clear();
        const DiagReport r = run(net);
        CHECK(ladder(r) == "FSSSSS" && r.step(Step::Link).detail == "no adapter" && has(r.explanation, "No network adapter"));
    }
    {
        FakeNet net("pi-5");
        for (Iface &i : net.st.ifaces) {
            if (i.name == "eth0") i.operstate = "down";
            if (i.name == "wlan0") i.wireless = false; /* a board with a cable only */
        }
        const DiagReport r = run(net);
        CHECK(ladder(r) == "FSSSSS" && has(r.explanation, "cable"));
    }

    // Address: none; self-assigned
    {
        FakeNet net;
        for (Iface &i : net.st.ifaces)
            if (i.name == "wlan0") i.ipv4.clear();
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PFSSSS" && r.step(Step::Address).detail == "no address" && has(r.explanation, "DHCP"));
    }
    {
        FakeNet net;
        for (Iface &i : net.st.ifaces)
            if (i.name == "wlan0") i.ipv4 = {"169.254.31.7/16"};
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PFSSSS" && has(r.explanation, "self-assigned address (169.254.31.7)"));
    }

    // Gateway: no default route; silent; ignores ping but answers on TCP
    {
        FakeNet net;
        net.st.route = Route();
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPFSSS" && r.step(Step::Gateway).detail == "no gateway" && has(r.explanation, "no way out"));
    }
    {
        FakeNet net;
        net.ping_answers.erase("192.168.68.1");
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPFSSS"));
        CHECK_EQ(r.step(Step::Gateway).detail, std::string("192.168.68.1 silent"));
        CHECK(has(r.explanation, "The gateway 192.168.68.1 does not answer"));
        CHECK(net.pings == 3); /* three tries */
    }
    {
        FakeNet net;
        net.ping_answers.erase("192.168.68.1");
        net.tcp_answers["192.168.68.1"] = {80};
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPPPPP"));
        CHECK_EQ(r.step(Step::Gateway).detail, std::string("192.168.68.1, 21 ms"));
        CHECK(r.notes.size() == 1 && has(r.notes[0], "ignores ping; it answered on TCP 80"));
    }

    // DNS: the first resolver silent, the second answers (the report names it)
    {
        FakeNet net;
        net.dns_silent.insert("192.168.1.1");
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPPPPP"));
        CHECK_EQ(r.resolver, std::string("192.168.68.1"));
        CHECK_EQ(r.step(Step::Dns).detail, std::string("via 192.168.68.1, 12 ms"));
        CHECK(r.notes.size() == 1 && has(r.notes[0], "192.168.1.1 timed out; 192.168.68.1 answered"));
        CHECK(net.asked_dns.back() == "192.168.68.1 connectivitycheck.gstatic.com"); /* the portal uses it too */
    }
    // DNS: all silent while the internet answers -> "try another resolver"
    {
        FakeNet net;
        net.dns_silent = {"192.168.1.1", "192.168.68.1"};
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPPFSS"));
        CHECK(r.failed == 3 && r.resolver == "192.168.1.1");
        CHECK_EQ(r.step(Step::Dns).detail, std::string("192.168.1.1 silent"));
        CHECK(has(r.explanation, "The gateway answers but DNS does not (192.168.1.1 timed out, 192.168.68.1 timed out)"));
        CHECK(has(r.explanation, "Try another resolver"));
    }
    // DNS: all silent and nothing past the gateway answers -> "internet down"
    {
        FakeNet net;
        net.dns_silent = {"192.168.1.1", "192.168.68.1"};
        net.ping_answers = {"192.168.68.1"};
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPFSS" && has(r.explanation, "Nothing answers past the gateway"));
    }
    // DNS: NXDOMAIN for a name that exists; no server at all; the user's resolver
    {
        FakeNet net;
        net.dns_nxdomain.insert("192.168.1.1");
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPFSS" && r.step(Step::Dns).detail == "192.168.1.1: NXDOMAIN");
        CHECK(has(r.explanation, "answered \"no such name\" for example.com"));
    }
    {
        FakeNet net;
        net.st.dns.servers.clear();
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPFSS" && r.step(Step::Dns).detail == "no DNS server" && has(r.explanation, "resolv.conf"));
    }
    {
        FakeNet net;
        DiagOptions opt;
        opt.resolver = "9.9.9.9";
        const DiagReport r = run(net, opt);
        CHECK(ladder(r) == "PPPPPP" && r.resolver == "9.9.9.9" && net.asked_dns[0] == "9.9.9.9 example.com");
    }

    // Internet: silent; ping blocked but TCP works; ping not allowed at all
    {
        FakeNet net;
        net.ping_answers = {"192.168.68.1"};
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPPPFS"));
        CHECK(r.step(Step::Internet).detail == "no answer" && has(r.explanation, "1.1.1.1 and 8.8.8.8 are silent"));
    }
    {
        FakeNet net;
        net.ping_answers = {"192.168.68.1"};
        net.tcp_answers["8.8.8.8"] = {443};
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPPPP" && r.step(Step::Internet).detail == "8.8.8.8, TCP 443, 21 ms");
        CHECK(r.notes.size() == 1 && has(r.notes[0], "blocked"));
    }
    {
        FakeNet net;
        net.ping_allowed = false;
        net.tcp_answers["192.168.68.1"] = {53};
        net.tcp_answers["1.1.1.1"] = {443};
        const DiagReport r = run(net);
        CHECK_EQ(ladder(r), std::string("PPPPPP"));
        CHECK(net.pings == 0);
        CHECK(r.notes.size() == 1 && has(r.notes[0], "Ping is not allowed"));
        CHECK_EQ(r.step(Step::Internet).detail, std::string("1.1.1.1, TCP 443, 21 ms"));
    }

    // Captive portal: a redirect; the check name does not resolve; port 80 blocked
    {
        FakeNet net;
        net.http_status = 302;
        net.http_location = "http://192.168.4.1/login";
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPPPF" && r.failed == 5 && r.step(Step::Portal).detail == "HTTP 302");
        CHECK(has(r.explanation, "(HTTP 302, to http://192.168.4.1/login)") && has(r.explanation, "sign in"));
    }
    {
        FakeNet net;
        net.http_status = 200;
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPPPF" && has(r.explanation, "captive portal"));
    }
    {
        FakeNet net;
        net.names.erase("connectivitycheck.gstatic.com");
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPPPF" && r.step(Step::Portal).detail == "cannot resolve");
    }
    {
        FakeNet net;
        net.http_answers = false;
        const DiagReport r = run(net);
        CHECK(ladder(r) == "PPPPPF" && has(r.explanation, "Web requests (port 80) do not get through (timed out)"));
    }

    // the session: a cancel while a step waits puts it back to pending
    {
        auto net = std::make_shared<FakeNet>();
        net->block_ping = true;
        DiagSession s(net);
        CHECK(s.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(s.running() && ladder(*s.latest()) == "PPR...");
        s.stop();
        CHECK(s.wait(1000));
        CHECK(s.latest()->state == DiagReport::State::Cancelled && ladder(*s.latest()) == "PP....");
        net->block_ping = false;
        CHECK(s.start() && s.wait(2000));
        CHECK(s.latest()->state == DiagReport::State::Passed);
    }
}
