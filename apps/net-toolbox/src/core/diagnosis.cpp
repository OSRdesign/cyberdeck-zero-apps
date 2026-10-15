/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the diagnosis (see diagnosis.hpp).
 */
#include "diagnosis.hpp"

#include "cmdrun.hpp"
#include "ping.hpp"
#include "scope.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <algorithm>

namespace nt {

const char *step_title(Step s)
{
    switch (s) {
    case Step::Link: return "Link up";
    case Step::Address: return "Address";
    case Step::Gateway: return "Gateway";
    case Step::Dns: return "DNS";
    case Step::Internet: return "Internet";
    case Step::Portal: return "Captive portal";
    }
    return "";
}

const char *step_state_name(StepState s)
{
    switch (s) {
    case StepState::Pending: return "pending";
    case StepState::Running: return "running";
    case StepState::Pass: return "pass";
    case StepState::Fail: return "fail";
    case StepState::Skipped: return "skipped";
    }
    return "";
}

DiagReport::DiagReport()
{
    for (int i = 0; i < kStepCount; ++i) {
        DiagStep s;
        s.id = static_cast<Step>(i);
        steps.push_back(s);
    }
}

/* ---------------------------------------------------------------- the real network */

namespace {

class SystemProbe : public DiagProbe {
public:
    explicit SystemProbe(std::shared_ptr<const Source> source) : source_(std::move(source)) {}
    NetState state() override
    {
        if (source_) return read_net_state(*source_);
        const std::unique_ptr<Source> sys = make_system_source();
        return read_net_state(*sys);
    }
    bool nmcli_dns(const std::string &iface, std::string &text) override
    {
        if (!valid_iface_name(iface)) return false;
        if (source_) { /* a fixture board: the recorded answer */
            bool ok = false;
            text = source_->read("/cmd/nmcli_dns_" + iface, &ok);
            return ok;
        }
        CommandSpec spec;
        spec.argv = nmcli_dns_argv(iface);
        spec.timeout_ms = kNmcliTimeoutMs;
        spec.max_output = 16 * 1024;
        DirectRunner runner;
        const CancelToken none;
        const CommandResult r = runner.run(spec, none);
        if (!r.ok()) return false;
        text = r.output;
        return true;
    }
    bool can_ping() override { return ping_available().ipv4; }
    bool ping(const Address &ip, int timeout_ms, const CancelToken &cancel, double *rtt_ms) override
    {
        int err = 0;
        std::unique_ptr<EchoSocket> sock = open_icmp_echo(ip, 56, &err);
        if (!sock || sock->send(1)) return false;
        const std::int64_t sent = now_us();
        const std::int64_t deadline = now_ms() + timeout_ms;
        while (!cancel.cancelled()) {
            const std::int64_t left = deadline - now_ms();
            if (left <= 0) return false;
            std::uint16_t seq = 0;
            const int r = sock->receive(static_cast<int>(std::min<std::int64_t>(left, kSliceMs)), seq);
            if (r < 0) return false;
            if (r == 1 && seq == 1) {
                if (rtt_ms) *rtt_ms = static_cast<double>(now_us() - sent) / 1000.0;
                return true;
            }
        }
        return false;
    }
    DnsResult dns(const std::string &name, std::uint16_t type, const std::string &server, int timeout_ms,
                  const CancelToken &cancel) override
    {
        DnsQuery q;
        q.name = name;
        q.type = type;
        q.server = server;
        q.timeout_ms = timeout_ms;
        q.tries = 2;
        return dns_query(q, cancel);
    }
    PortResult tcp(const Address &ip, std::uint16_t port, int timeout_ms, const CancelToken &cancel) override
    {
        return check_port(ip, port, timeout_ms, cancel);
    }
    HttpResult http(const Address &ip, const std::string &host, const std::string &path, int timeout_ms,
                    const CancelToken &cancel) override
    {
        return http_get(ip, 80, host, path, timeout_ms, cancel);
    }

private:
    std::shared_ptr<const Source> source_;
};

class Run {
public:
    Run(DiagProbe &probe, const DiagOptions &opt, const CancelToken &cancel,
        const std::function<void(const DiagReport &)> &publish)
        : probe_(probe), opt_(opt), cancel_(cancel), publish_(publish)
    {
    }

    DiagReport go()
    {
        r_.state = DiagReport::State::Running;
        emit();
        if (!link() || !address() || !gateway() || !dns() || !internet() || !portal()) return r_;
        r_.state = DiagReport::State::Passed;
        r_.explanation = scoped() ? "Your connection through " + opt_.iface +
                                        " works: link, address, gateway, DNS and internet, and no sign-in page."
                                  : "Your connection works: link, address, gateway, DNS and internet, and no sign-in page.";
        emit();
        return r_;
    }

private:
    bool scoped() const { return !opt_.iface.empty(); }
    DiagStep &at(Step s) { return r_.steps[static_cast<std::size_t>(s)]; }
    void emit()
    {
        if (publish_) publish_(r_);
    }
    void begin(Step s)
    {
        at(s).state = StepState::Running;
        emit();
    }
    bool pass(Step s, const std::string &detail)
    {
        at(s).state = StepState::Pass;
        at(s).detail = detail;
        emit();
        return true;
    }
    bool fail(Step s, const std::string &detail, const std::string &why)
    {
        at(s).state = StepState::Fail;
        at(s).detail = detail;
        for (int i = static_cast<int>(s) + 1; i < kStepCount; ++i) r_.steps[static_cast<std::size_t>(i)].state = StepState::Skipped;
        r_.failed = static_cast<int>(s);
        r_.state = DiagReport::State::Failed;
        r_.explanation = why;
        emit();
        return false;
    }
    /* a cancel ends the run: the running step goes back to pending */
    bool stopped()
    {
        if (!cancel_.cancelled()) return false;
        for (DiagStep &s : r_.steps)
            if (s.state == StepState::Running) s.state = StepState::Pending;
        r_.state = DiagReport::State::Cancelled;
        emit();
        return true;
    }

    /* an address answers: ping (tries), else TCP to one of the ports (open or refused both prove it is there) */
    bool reach(const Address &ip, int pings, const std::vector<std::uint16_t> &ports, std::string &how, double &rtt)
    {
        if (ping_ok_) {
            for (int i = 0; i < pings && !cancel_.cancelled(); ++i)
                if (probe_.ping(ip, opt_.ping_timeout_ms, cancel_, &rtt)) {
                    how = "ping";
                    return true;
                }
        }
        for (const std::uint16_t port : ports) {
            if (cancel_.cancelled()) return false;
            const PortResult pr = probe_.tcp(ip, port, opt_.tcp_timeout_ms, cancel_);
            if (pr.state == PortState::Open || pr.state == PortState::Closed) {
                how = "TCP " + std::to_string(port);
                rtt = pr.connect_ms;
                return true;
            }
        }
        return false;
    }

    bool link()
    {
        begin(Step::Link);
        st_ = probe_.state();
        if (stopped()) return false;
        if (scoped()) return link_scoped();
        main_ = st_.main();
        if (!main_ || !main_->connected())
            for (const Iface &i : st_.ifaces)
                if (i.connected()) {
                    main_ = &i;
                    break;
                }
        if (main_ && main_->connected()) {
            std::string detail = main_->name;
            if (main_->link_valid && !main_->link.ssid.empty()) detail += " " + main_->link.ssid;
            return pass(Step::Link, detail);
        }
        const Iface *wifi = nullptr;
        const Iface *wired = nullptr;
        for (const Iface &i : st_.ifaces) {
            if (i.loopback) continue;
            if (i.wireless && !wifi) wifi = &i;
            if (!i.wireless && !wired) wired = &i;
        }
        if (!wifi && !wired)
            return fail(Step::Link, "no adapter",
                        "No network adapter was found (no Wi-Fi, no Ethernet). A USB adapter may be unplugged.");
        if (wifi)
            return fail(Step::Link, wifi->name + " not connected",
                        "Wi-Fi is not connected to a network. Pick a network in Settings > Wi-Fi, or plug in a cable.");
        return fail(Step::Link, wired->name + " down",
                    "No network link: the cable is unplugged or the other end is off. Check the cable and the router.");
    }

    /* a chosen network: that interface, and nothing else */
    bool link_scoped()
    {
        const std::string &name = opt_.iface;
        r_.iface = name;
        const Iface *i = st_.find(name);
        if (!i || i->loopback)
            return fail(Step::Link, name + " not found",
                        name + " was not found: a USB adapter may be unplugged. Plug it in, or choose Automatic or another "
                               "network in Home > Network.");
        main_ = i;
        if (i->connected()) {
            std::string detail = name;
            if (i->link_valid && !i->link.ssid.empty()) detail += " " + i->link.ssid;
            else if (!i->wireless && i->speed_mbps > 0) detail += " " + fmt_speed(i->speed_mbps);
            return pass(Step::Link, detail);
        }
        if (i->wireless)
            return fail(Step::Link, name + " not connected",
                        name + " is not connected to a network. Connect it to one, or choose another network in "
                               "Home > Network.");
        return fail(Step::Link, name + " no link",
                    name + " has no link: the cable is unplugged or the other end is off. Check the cable, the hub and "
                           "the router.");
    }

    bool address()
    {
        begin(Step::Address);
        const std::string ip = main_->first_ipv4();
        Address a;
        if (ip.empty() || !parse_ip(ip, a))
            return fail(Step::Address, "no address",
                        "Connected to " + main_->name + " but no address was given (DHCP). Wait a moment or reconnect; "
                        "if it lasts, restart the router.");
        if (is_ipv4_link_local(a))
            return fail(Step::Address, ip,
                        "Only a self-assigned address (" + ip + (scoped() ? ") on " + main_->name : std::string(")")) +
                            ": the router did not answer the address request (DHCP). Reconnect, or restart the router.");
        return pass(Step::Address, ip);
    }

    bool gateway()
    {
        begin(Step::Gateway);
        ping_ok_ = probe_.can_ping();
        if (!ping_ok_) r_.notes.push_back("Ping is not allowed for this user: the checks used TCP connections instead.");
        const Route route = scoped() ? st_.route_of(opt_.iface) : st_.route;
        if (!route.ok)
            return fail(Step::Gateway, "no gateway",
                        scoped() ? opt_.iface + " has an address but no gateway (default route): this network gives no way "
                                               "out. Reconnect, or check the router."
                                 : "There is an address but no gateway (default route): this network gives no way out. "
                                   "Reconnect, or check the router.");
        const std::string gw = route.gateway;
        Address a;
        if (!parse_ip(gw, a)) return fail(Step::Gateway, gw, "The default route names no usable gateway address.");
        std::string how;
        double rtt = 0;
        const bool ok = reach(a, opt_.gateway_pings, {53, 80, 443}, how, rtt);
        if (stopped()) return false;
        if (!ok)
            return fail(Step::Gateway, gw + " silent",
                        (scoped() ? "The gateway of " + opt_.iface + " (" + gw + ")" : "The gateway " + gw) +
                            " does not answer: the link may be too weak or the router is stuck. "
                            "Move closer, or restart the router.");
        if (how != "ping" && ping_ok_) r_.notes.push_back("The gateway ignores ping; it answered on " + how + ".");
        return pass(Step::Gateway, gw + ", " + fmt_ms(rtt));
    }

    bool dns()
    {
        begin(Step::Dns);
        std::vector<std::string> servers;
        if (!opt_.resolver.empty()) {
            servers.push_back(opt_.resolver);
        } else if (scoped()) {
            std::string text;
            const bool ran = probe_.nmcli_dns(opt_.iface, text);
            const ScopeDns sd = scope_dns(resolve_scope(opt_.iface, st_), st_, text, ran);
            for (const std::string &s : sd.servers)
                if (is_ip(s) && servers.size() < 3) servers.push_back(s);
            if (sd.from == ScopeDns::From::Gateway)
                r_.notes.push_back("The DNS servers of " + opt_.iface + " could not be read: its gateway was asked.");
            else if (sd.from == ScopeDns::From::SystemFallback)
                r_.notes.push_back("The DNS servers of " + opt_.iface + " are not known: the system's were asked, which "
                                   "may not be reachable through it.");
        } else {
            for (const std::string &s : st_.dns.servers)
                if (is_ip(s) && servers.size() < 3) servers.push_back(s);
        }
        if (servers.empty())
            return fail(Step::Dns, "no DNS server",
                        scoped() ? "No DNS server is known for " + opt_.iface + ". Reconnect it, or pick a resolver such as "
                                   "1.1.1.1 in DNS lookup."
                                 : "No DNS server is set (resolv.conf lists none). Reconnect, or pick a resolver such as "
                                   "1.1.1.1 in DNS lookup.");
        r_.resolver = servers.front();
        std::vector<std::string> silent;
        DnsResult answer;
        bool answered = false;
        for (const std::string &s : servers) {
            answer = probe_.dns(opt_.dns_name, dns::kA, s, opt_.dns_timeout_ms, cancel_);
            if (stopped()) return false;
            if (answer.answered) {
                answered = true;
                r_.resolver = s;
                break;
            }
            silent.push_back(s + " " + (answer.error.empty() ? std::string("timed out") : answer.error));
        }
        if (!answered) {
            std::string list;
            for (const std::string &s : silent) list += (list.empty() ? "" : ", ") + s;
            /* is it DNS, or is everything past the gateway down? one quick look decides the advice */
            bool internet = false;
            Address probe_ip;
            if (!opt_.internet.empty() && parse_ip(opt_.internet.front(), probe_ip)) {
                std::string how;
                double rtt = 0;
                internet = reach(probe_ip, 1, {443}, how, rtt);
            }
            if (stopped()) return false;
            if (internet)
                return fail(Step::Dns, servers.front() + " silent",
                            std::string(scoped() ? "The gateway of " + opt_.iface + " answers" : "The gateway answers") +
                            " but DNS does not (" + list + "). Try another resolver, for example 1.1.1.1, in DNS lookup.");
            return fail(Step::Dns, servers.front() + " silent",
                        std::string(scoped() ? "Nothing answers past the gateway of " + opt_.iface
                                             : "Nothing answers past the gateway") +
                            " (DNS: " + list + "): the router's internet connection seems down. " +
                            (scoped() ? "Check the modem, the hotspot or the router, or restart it."
                                      : "Check the modem or the router, or restart it."));
        }
        if (!silent.empty()) r_.notes.push_back("DNS: " + silent.front() + "; " + r_.resolver + " answered.");
        const bool has_address = !answer.addresses(dns::kA).empty();
        if (answer.rcode != dns::kNoError || !has_address) {
            const std::string what = answer.rcode != dns::kNoError ? answer.rcode_name : std::string("no address");
            const std::string words = answer.rcode != dns::kNoError ? answer.rcode_words : std::string("no address");
            return fail(Step::Dns, r_.resolver + ": " + what,
                        "The DNS server " + r_.resolver + " answered \"" + words + "\" for " + opt_.dns_name +
                        ", a name that exists: it is filtering or broken. Try another resolver, for example 1.1.1.1.");
        }
        return pass(Step::Dns, "via " + r_.resolver + ", " + fmt_ms(answer.elapsed_ms));
    }

    bool internet()
    {
        begin(Step::Internet);
        std::vector<Address> targets;
        for (const std::string &t : opt_.internet) {
            Address a;
            if (parse_ip(t, a)) targets.push_back(a);
        }
        if (ping_ok_) {
            for (const Address &a : targets) {
                for (int i = 0; i < opt_.internet_pings; ++i) {
                    double rtt = 0;
                    if (probe_.ping(a, opt_.ping_timeout_ms, cancel_, &rtt))
                        return pass(Step::Internet, a.text() + ", " + fmt_ms(rtt));
                    if (stopped()) return false;
                }
            }
        }
        for (const Address &a : targets) { /* ping not allowed, or blocked on the way */
            const PortResult pr = probe_.tcp(a, 443, opt_.tcp_timeout_ms, cancel_);
            if (stopped()) return false;
            if (pr.state == PortState::Open || pr.state == PortState::Closed) {
                if (ping_ok_) r_.notes.push_back("Ping to the internet is blocked on this network; TCP works.");
                return pass(Step::Internet, a.text() + ", TCP 443, " + fmt_ms(pr.connect_ms));
            }
        }
        std::string list;
        for (const std::string &t : opt_.internet) list += (list.empty() ? "" : " and ") + t;
        return fail(Step::Internet, "no answer",
                    scoped() ? "The gateway of " + opt_.iface + " answers but the internet does not (" + list +
                                   " are silent). The internet connection behind it may be down: check the modem or the "
                                   "hotspot, or wait and run again."
                             : "The router answers but the internet does not (" + list + " are silent). The router's internet "
                               "connection may be down: check the modem, or wait and run again.");
    }

    bool portal()
    {
        begin(Step::Portal);
        const DnsResult r = probe_.dns(opt_.portal_host, dns::kA, r_.resolver, opt_.dns_timeout_ms, cancel_);
        if (stopped()) return false;
        const std::vector<std::string> addrs = r.addresses(dns::kA);
        Address a;
        if (!r.ok() || addrs.empty() || !parse_ip(addrs.front(), a)) {
            const std::string why = !r.answered ? r.error : r.rcode != dns::kNoError ? r.rcode_words : "no address";
            return fail(Step::Portal, "cannot resolve",
                        "The check server " + opt_.portal_host + " cannot be found (" + why + "). A network that "
                        "blocks names before you sign in does this: open a browser on another device to sign in.");
        }
        const HttpResult h = probe_.http(a, opt_.portal_host, opt_.portal_path, opt_.http_timeout_ms, cancel_);
        if (stopped()) return false;
        if (!h.answered)
            return fail(Step::Portal, h.error.empty() ? std::string("no answer") : h.error,
                        "Web requests (port 80) do not get through (" + (h.error.empty() ? std::string("no answer") : h.error) +
                            "). A firewall may block them; other services may still work.");
        if (h.status == 204) return pass(Step::Portal, "none (204)");
        std::string where;
        if (!h.location.empty()) where = ", to " + h.location;
        return fail(Step::Portal, "HTTP " + std::to_string(h.status),
                    "This network answers web requests with its own page (HTTP " + std::to_string(h.status) + where +
                        "): it probably wants you to sign in first (captive portal). Sign in from a phone or a "
                        "computer on the same network, then run again.");
    }

    DiagProbe &probe_;
    const DiagOptions &opt_;
    const CancelToken &cancel_;
    const std::function<void(const DiagReport &)> &publish_;
    DiagReport r_;
    NetState st_;
    const Iface *main_ = nullptr;
    bool ping_ok_ = true;
};

} // namespace

std::shared_ptr<DiagProbe> make_system_probe(std::shared_ptr<const Source> source)
{
    return std::make_shared<SystemProbe>(std::move(source));
}

DiagReport run_diagnosis(DiagProbe &probe, const DiagOptions &opt, const CancelToken &cancel,
                         const std::function<void(const DiagReport &)> &publish)
{
    Run run(probe, opt, cancel, publish);
    return run.go();
}

DiagSession::DiagSession(std::shared_ptr<DiagProbe> probe) : probe_(std::move(probe)) {}

bool DiagSession::start(const DiagOptions &opt)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    DiagReport first;
    first.state = DiagReport::State::Running;
    channel_.begin(gen, first);
    const std::shared_ptr<DiagProbe> probe = probe_;
    const std::uint64_t started = worker_.start([this, gen, opt, probe](const CancelToken &cancel) {
        run_diagnosis(*probe, opt, cancel, [this, gen](const DiagReport &r) { channel_.publish(gen, r); });
    });
    if (!started) {
        first.state = DiagReport::State::Failed;
        first.explanation = "Could not start the diagnosis (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void DiagSession::stop()
{
    worker_.cancel();
}

} // namespace nt
