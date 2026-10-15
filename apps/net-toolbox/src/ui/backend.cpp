/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the real runners and the demonstration ones (see backend.hpp).
 */
#include "backend.hpp"

#include "demo_data.hpp"
#include "demo_data_03.hpp"
#include "flow_runner.hpp"
#include "netaddr.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace nt {
namespace ui {

NetState Backend::read_state() const
{
    if (source) return read_net_state(*source);
    const std::unique_ptr<Source> sys = make_system_source();
    return read_net_state(*sys);
}

bool Backend::fixture_link(const std::string &iface, IwLink &link) const
{
    if (!source) return false;
    bool ok = false;
    const std::string text = source->read("/cmd/iw_dev_" + iface + "_link", &ok);
    if (!ok) return false;
    link = parse_iw_link(text);
    return true;
}

bool Backend::fixture_nmcli(const std::string &iface, std::string &text) const
{
    if (!source || !valid_iface_name(iface)) return false;
    bool ok = false;
    text = source->read("/cmd/nmcli_dns_" + iface, &ok);
    return ok;
}

namespace {

/* ---------------------------------------------------------------- the real tools */

template <class Session, class Req, class Snap> class RealRunner : public SessionRunner<Session, Req, Snap> {
public:
    RealRunner() : SessionRunner<Session, Req, Snap>() {}
    explicit RealRunner(std::shared_ptr<DiagProbe> probe) : SessionRunner<Session, Req, Snap>(std::move(probe)) {}
};

/* ---------------------------------------------------------------- the demonstration: options */

std::map<std::string, std::string> parse_spec(const std::string &spec)
{
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string part = spec.substr(pos, end - pos);
        const std::size_t eq = part.find('=');
        if (eq != std::string::npos) out[trim(part.substr(0, eq))] = trim(part.substr(eq + 1));
        else if (!trim(part).empty()) out[trim(part)] = "";
        pos = end + 1;
    }
    return out;
}

bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

/* ---------------------------------------------------------------- the demonstration: the diagnosis */

class DemoProbe : public DiagProbe {
public:
    DemoProbe(std::shared_ptr<const Source> source, std::string scenario)
        : source_(std::move(source)), scenario_(std::move(scenario))
    {
    }
    NetState state() override { return read_net_state(*source_); }
    bool nmcli_dns(const std::string &iface, std::string &text) override
    {
        if (!valid_iface_name(iface)) return false;
        bool ok = false;
        text = source_->read("/cmd/nmcli_dns_" + iface, &ok);
        return ok;
    }
    bool can_ping() override { return true; }
    static bool is_gateway(const NetState &st, const Address &ip)
    {
        for (const Route &r : st.default_routes)
            if (ip.text() == r.gateway) return true;
        return false;
    }
    bool ping(const Address &ip, int, const CancelToken &, double *rtt) override
    {
        const NetState st = state();
        const bool gateway = is_gateway(st, ip);
        if (gateway && scenario_ == "notes") return false; /* a router that ignores ping but answers on TCP */
        if (rtt) *rtt = gateway ? 3.0 : ip.text() == "1.1.1.1" ? 14.0 : 19.0;
        return true;
    }
    DnsResult dns(const std::string &name, std::uint16_t type, const std::string &server, int,
                  const CancelToken &) override
    {
        DnsResult r;
        r.name = name;
        r.type = type;
        r.server = server;
        if (scenario_ == "dns-fail") {
            r.error = "timed out";
            r.elapsed_ms = 2000;
            return r;
        }
        r.answered = true;
        r.rcode = dns::kNoError;
        r.rcode_name = "NOERROR";
        r.rcode_words = "OK";
        r.elapsed_ms = 12;
        dns::Record rec;
        rec.name = name;
        rec.type = dns::kA;
        rec.ttl = 300;
        rec.data = name == "example.com" ? "93.184.215.14" : "142.250.179.99";
        r.answers.push_back(rec);
        return r;
    }
    PortResult tcp(const Address &ip, std::uint16_t port, int, const CancelToken &) override
    {
        PortResult r;
        r.port = port;
        const NetState st = state();
        const bool gateway = is_gateway(st, ip);
        if (gateway && scenario_ == "notes" && port == 53) {
            r.state = PortState::Open;
            r.connect_ms = 4;
        } else {
            r.state = PortState::Filtered;
            r.detail = "timed out";
        }
        return r;
    }
    HttpResult http(const Address &, const std::string &, const std::string &, int, const CancelToken &) override
    {
        HttpResult h;
        h.answered = true;
        h.elapsed_ms = 61;
        if (scenario_ == "portal") {
            h.status = 302;
            h.location = "http://login.hotspot.example/portal";
        } else {
            h.status = 204;
        }
        return h;
    }

private:
    std::shared_ptr<const Source> source_;
    std::string scenario_;
};

DiagReport demo_diag(const std::shared_ptr<const Source> &source, const std::string &scenario, const DiagOptions &opt)
{
    DemoProbe probe(source, scenario);
    std::vector<DiagReport> frames;
    const CancelToken none;
    const DiagReport last = run_diagnosis(probe, opt, none, [&](const DiagReport &r) { frames.push_back(r); });
    if (scenario == "running") /* the frame where the DNS rung is being checked */
        for (const DiagReport &f : frames)
            if (f.step(Step::Dns).state == StepState::Running) return f;
    return last;
}

/* ---------------------------------------------------------------- the demonstration: ping */

PingSnapshot demo_ping(const std::string &scenario, const PingConfig &cfg)
{
    PingSnapshot s;
    s.target = cfg.target;
    s.address = is_ip(cfg.target) ? cfg.target : "93.184.215.14";
    if (!is_ip(cfg.target)) s.resolved_by = "192.168.1.1";
    if (scenario == "failed") {
        s.state = PingSnapshot::State::Failed;
        s.address.clear();
        s.resolved_by.clear();
        s.error = cfg.target + " does not exist (NXDOMAIN)";
        return s;
    }
    const bool lost_all = scenario == "lost";
    const int n = lost_all ? 30 : 120;
    PacketRing ring;
    for (int k = 1; k <= n; ++k) {
        s.stats.add_sent();
        PacketResult p;
        p.number = static_cast<std::uint32_t>(k);
        const bool lost = lost_all || k == 17 || k == 18 || k == 53 || k == 91;
        const bool pending = !lost_all && scenario != "stopped" && k > n - 1;
        if (pending) {
            p.state = PacketState::Pending;
        } else if (lost) {
            s.stats.add_lost();
            p.state = PacketState::Lost;
        } else {
            double rtt = 10.5 + ((k * 37) % 11) * 0.55 + (k % 7 == 0 ? 3.5 : 0.0);
            if (k == 40) rtt = 186.0;
            if (k == 41) rtt = 243.0;
            if (k == 76) rtt = 131.0;
            p.rtt_ms = rtt;
            p.state = s.stats.add_reply(rtt);
        }
        ring.push(p);
    }
    s.recent = ring.items();
    s.state = scenario == "stopped" ? PingSnapshot::State::Cancelled : PingSnapshot::State::Running;
    return s;
}

/* ---------------------------------------------------------------- the demonstration: DNS */

dns::Record record(const std::string &name, std::uint16_t type, std::uint32_t ttl, const std::string &data)
{
    dns::Record r;
    r.name = name;
    r.type = type;
    r.ttl = ttl;
    r.data = data;
    return r;
}

DnsResult demo_dns_one(const std::string &qname, std::uint16_t type, const std::string &server, int index,
                       const std::string &input)
{
    DnsResult r;
    r.name = qname;
    r.type = type;
    r.server = server;
    r.chain = {qname};
    if (starts(input, "slow") || starts(input, "timeout")) {
        if (index % 2 == 0) {
            r.error = "timed out";
            r.elapsed_ms = 4000;
            return r;
        }
    }
    static const int times[] = {12, 9, 21, 34, 17, 25};
    r.answered = true;
    r.elapsed_ms = times[index % 6];
    r.tries_used = 1;
    if (starts(input, "nx")) {
        r.rcode = dns::kNxDomain;
        r.rcode_name = "NXDOMAIN";
        r.rcode_words = "no such name";
        return r;
    }
    r.rcode = dns::kNoError;
    r.rcode_name = "NOERROR";
    r.rcode_words = "OK";
    std::string target = qname;
    if (starts(qname, "www.")) {
        target = qname.substr(4);
        r.answers.push_back(record(qname, dns::kCNAME, 3600, target));
        r.chain = {qname, target};
    }
    switch (type) {
    case dns::kA: r.answers.push_back(record(target, dns::kA, 300, "93.184.215.14")); break;
    case dns::kAAAA: r.answers.push_back(record(target, dns::kAAAA, 300, "2606:2800:21f:cb07:6820:80da:af6b:8b2c")); break;
    case dns::kMX:
        r.answers.push_back(record(target, dns::kMX, 3600, "10 mail." + target));
        r.answers.push_back(record(target, dns::kMX, 3600, "20 mail2." + target));
        break;
    case dns::kTXT: r.answers.push_back(record(target, dns::kTXT, 300, "\"v=spf1 -all\"")); break;
    case dns::kNS:
        r.answers.push_back(record(target, dns::kNS, 86400, "a.iana-servers.net"));
        r.answers.push_back(record(target, dns::kNS, 86400, "b.iana-servers.net"));
        break;
    case dns::kPTR: r.answers.push_back(record(target, dns::kPTR, 3600, "one.one.one.one")); break;
    default: r.answers.push_back(record(target, dns::kCNAME, 300, "alias." + target)); break;
    }
    return r;
}

DnsSnapshot demo_dns(const DnsRequest &req)
{
    DnsSnapshot s;
    std::uint16_t type = req.type;
    std::string qname, why;
    if (req.servers.empty()) {
        s.state = DnsSnapshot::State::Failed;
        s.error = "No resolver to ask: the system has none, type one";
        return s;
    }
    if (!make_question(req.input, type, qname, &why)) {
        s.state = DnsSnapshot::State::Failed;
        s.error = why;
        return s;
    }
    s.question = qname;
    s.type = type;
    s.servers = req.servers;
    int index = 0;
    for (const std::string &server : req.servers) s.results.push_back(demo_dns_one(qname, type, server, index++, req.input));
    s.state = DnsSnapshot::State::Done;
    return s;
}

/* ---------------------------------------------------------------- the demonstration: port check */

PortSnapshot demo_port(const PortRequest &req)
{
    PortSnapshot s;
    s.host = req.host;
    if (starts(req.host, "nx")) {
        s.state = PortSnapshot::State::Failed;
        s.error = req.host + " does not exist (NXDOMAIN)";
        return s;
    }
    s.address = is_ip(req.host) ? req.host : "93.184.215.14";
    for (const std::uint16_t port : req.ports) {
        PortResult r;
        r.port = port;
        switch (port) {
        case 22:
            r.state = PortState::Closed;
            r.connect_ms = 3;
            r.detail = "connection refused";
            break;
        case 25:
            r.state = PortState::Filtered;
            r.detail = "timed out";
            break;
        case 3389:
            r.state = PortState::Unreachable;
            r.detail = "no route to host";
            break;
        default:
            r.state = PortState::Open;
            r.connect_ms = port == 443 ? 19 : 14;
            break;
        }
        s.results.push_back(r);
    }
    s.state = PortSnapshot::State::Done;
    return s;
}

} // namespace

Backend make_demo_backend(const std::string &spec, std::shared_ptr<const Source> source)
{
    const std::map<std::string, std::string> opt = parse_spec(spec);
    const auto get = [&](const char *key, const char *fallback) {
        const auto it = opt.find(key);
        return it == opt.end() || it->second.empty() ? std::string(fallback) : it->second;
    };
    Backend b;
    b.demo = true;
    b.source = std::move(source);
    b.description = "demonstration " + spec;
    const std::string diag = get("diag", "healthy");
    const std::string ping = get("ping", "mixed");
    const std::shared_ptr<const Source> src = b.source;
    b.diag = std::make_unique<FixedRunner<DiagOptions, DiagReport>>(
        [src, diag](const DiagOptions &opt) { return demo_diag(src, diag, opt); },
        [](const DiagReport &now) {
            DiagReport r = now;
            r.state = DiagReport::State::Cancelled;
            return r;
        });
    b.ping = std::make_unique<FixedRunner<PingConfig, PingSnapshot>>(
        [ping](const PingConfig &cfg) { return demo_ping(ping, cfg); },
        [](const PingSnapshot &now) {
            PingSnapshot s = now;
            s.state = PingSnapshot::State::Cancelled;
            return s;
        });
    b.dns = std::make_unique<FixedRunner<DnsRequest, DnsSnapshot>>([](const DnsRequest &req) { return demo_dns(req); });
    b.port = std::make_unique<FixedRunner<PortRequest, PortSnapshot>>([](const PortRequest &req) { return demo_port(req); });
    const std::string trace = get("trace", "healthy"), http = get("http", "ok"), pubip = get("pubip", "both"),
                      whois = get("whois", "domain");
    b.trace = std::make_unique<FixedRunner<TraceConfig, TraceSnapshot>>(
        [trace](const TraceConfig &cfg) { return demo_trace(trace, cfg); },
        [](const TraceSnapshot &now) {
            TraceSnapshot s = now;
            if (s.running()) s.state = TraceSnapshot::State::Cancelled;
            return s;
        });
    b.http = std::make_unique<FixedRunner<HttpCheckRequest, HttpCheckSnapshot>>(
        [http](const HttpCheckRequest &req) { return demo_http(http, req); },
        [](const HttpCheckSnapshot &now) {
            HttpCheckSnapshot s = now;
            if (s.running()) {
                s.state = HttpCheckSnapshot::State::Cancelled;
                s.step.clear();
            }
            return s;
        });
    b.pubip = std::make_unique<FixedRunner<NoRequest, PublicIpSnapshot>>(
        [pubip](const NoRequest &) { return demo_pubip(pubip); },
        [](const PublicIpSnapshot &now) {
            PublicIpSnapshot s = now;
            if (s.running()) s.state = PublicIpSnapshot::State::Cancelled;
            return s;
        });
    b.whois = std::make_unique<FixedRunner<WhoisRequest, WhoisSnapshot>>(
        [whois](const WhoisRequest &req) { return demo_whois(whois, req); },
        [](const WhoisSnapshot &now) {
            WhoisSnapshot s = now;
            if (s.running()) s.state = WhoisSnapshot::State::Cancelled;
            return s;
        });
    const std::string speed = get("speed", "idle"), wol = get("wol", "awake"), ble = get("ble", "scan25");
    b.speed = std::make_unique<FixedRunner<SpeedConfig, SpeedSnapshot>>(
        [speed](const SpeedConfig &cfg) { return demo_speed(speed, cfg); },
        [](const SpeedSnapshot &now) { return demo_speed_stop(now); });
    b.wol = std::make_unique<FixedRunner<WolRequest, WolSnapshot>>(
        [wol](const WolRequest &req) { return demo_wol(wol, req); }, [](const WolSnapshot &now) { return demo_wol_stop(now); });
    b.ble = std::make_unique<FixedRunner<BleConfig, BleSnapshot>>(
        [ble](const BleConfig &cfg) { return demo_ble(ble, cfg); }, [](const BleSnapshot &now) { return demo_ble_stop(now); });
    b.clock_ms = [] { return kDemoClockMs; };
    b.wall_clock = [] { return demo_now(); };
    b.local_ip = get("local", pubip == "cgnat" ? "100.64.8.3" : "");
    b.ping_available = [ping] {
        PingAvailability a;
        if (ping == "unavailable") {
            a.error = EACCES;
            a.why = ping_unavailable_words(EACCES, "1 0");
            return a;
        }
        a.ipv4 = a.ipv6 = true;
        return a;
    };
    for (const char *key : {"ping_target", "dns_name", "dns_type", "dns_resolver", "port_host", "port", "port_preset", "trace_target",
                            "http_url", "http_method", "http_insecure", "whois_query", "interface", "wol_machines",
                            "wol_last"}) {
        const auto it = opt.find(key);
        if (it != opt.end()) b.seeds[key] = it->second;
    }
    const std::string saved = get("saved", "");
    std::size_t pos = 0;
    while (pos < saved.size()) {
        std::size_t end = saved.find('+', pos);
        if (end == std::string::npos) end = saved.size();
        if (end > pos) b.saved_targets.push_back(saved.substr(pos, end - pos));
        pos = end + 1;
    }
    return b;
}

Backend make_backend()
{
    const char *demo = std::getenv("NET_TOOLBOX_DEMO");
    const bool is_demo = demo && demo[0];
    if (is_demo) setenv("NET_TOOLBOX_FIXTURE", "deck", 0); /* a demonstration reads a recorded board, never this one */
    std::string described;
    std::shared_ptr<const Source> source = source_from_env(&described);
    if (is_demo) {
        Backend b = make_demo_backend(demo, source);
        b.description += " on " + described;
        return b;
    }
    Backend b;
    b.source = source;
    b.description = "the real network, interfaces from " + described;
    b.diag = std::make_unique<RealRunner<DiagSession, DiagOptions, DiagReport>>(make_system_probe(source));
    b.ping = std::make_unique<RealRunner<PingSession, PingConfig, PingSnapshot>>();
    b.dns = std::make_unique<RealRunner<DnsSession, DnsRequest, DnsSnapshot>>();
    b.port = std::make_unique<RealRunner<PortSession, PortRequest, PortSnapshot>>();
    b.trace = std::make_unique<RealRunner<TraceSession, TraceConfig, TraceSnapshot>>();
    b.http = std::make_unique<AsyncHttpRunner>();
    b.pubip = std::make_unique<AsyncPublicIpRunner>();
    b.whois = std::make_unique<RealRunner<WhoisSession, WhoisRequest, WhoisSnapshot>>();
    b.speed = std::make_unique<RealRunner<SpeedTestSession, SpeedConfig, SpeedSnapshot>>();
    b.wol = std::make_unique<RealRunner<WolSession, WolRequest, WolSnapshot>>();
    b.ble = std::make_unique<RealRunner<BleSession, BleConfig, BleSnapshot>>();
    b.clock_ms = [] { return now_ms(); };
    b.wall_clock = [] { return std::time(nullptr); };
    b.ping_available = [] { return ping_available(); };
    return b;
}

} // namespace ui
} // namespace nt
