/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: "Why is my internet broken?" (no LVGL; tests/test_diagnosis.cpp runs it on a fake network).
 *
 * Six steps in order, stopping at the first failure (the steps after it are Skipped):
 *   Link up         an interface other than lo is up with a link (the default route's first)
 *   Address         it has an IPv4 address that is not self-assigned (169.254.x.x)
 *   Gateway         the default gateway answers ping; a gateway that ignores ping but answers on TCP 53, 80 or 443
 *                   (even with a refusal) passes too, with a note
 *   DNS             a fixed well-known name (example.com) resolves through the resolver in use: the system's servers
 *                   in resolv.conf order (the next one is asked when one is silent, as the C library does), or the one
 *                   the user chose; the report names the resolver that answered. When none answers, one ping to
 *                   1.1.1.1 tells "DNS down" from "internet down" for the explanation.
 *   Internet        1.1.1.1, then 8.8.8.8 answer ping; when ping is not allowed or blocked, a TCP connection to port
 *                   443 of the same addresses counts (with a note)
 *   Captive portal  a plain HTTP GET of http://connectivitycheck.gstatic.com/generate_204: 204 = no portal; any other
 *                   answer = a portal is suspected (it shows its own page)
 * With a chosen network (scope.hpp, DiagOptions::iface) the steps check THAT interface: Link up for it (a Wi-Fi adapter
 * that is not connected, a cable that is unplugged), its own address, ITS gateway (the default route through it, not the
 * system's), DNS through its own servers (nmcli, else its gateway, else the system's), and the internet and the portal
 * through sockets bound to it (sock.hpp). The explanations name the interface. Automatic: as described above.
 * Each step has a state, a short detail ("192.168.68.1, 3 ms") and the failed one a plain-words explanation with what to
 * do. All network access goes through a DiagProbe (the system, or a fake in the tests).
 */
#pragma once

#include "dns_client.hpp"
#include "http.hpp"
#include "interfaces.hpp"
#include "portcheck.hpp"
#include "source.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

enum class Step { Link, Address, Gateway, Dns, Internet, Portal };
constexpr int kStepCount = 6;

enum class StepState { Pending, Running, Pass, Fail, Skipped };

const char *step_title(Step s);            /* "Link up", "Address", "Gateway", "DNS", "Internet", "Captive portal" */
const char *step_state_name(StepState s);  /* "pending", "running", "pass", "fail", "skipped" */

struct DiagStep {
    Step id = Step::Link;
    StepState state = StepState::Pending;
    std::string detail;
};

struct DiagReport {
    enum class State { Idle, Running, Passed, Failed, Cancelled };
    State state = State::Idle;
    std::vector<DiagStep> steps;     /* kStepCount, in order */
    int failed = -1;                 /* the index of the failed step */
    std::string explanation;         /* the failure in plain words and what to do; or the all-good line */
    std::string resolver;            /* the DNS server that answered (or the first one asked) */
    std::string iface;               /* a chosen network: the interface checked ("" = Automatic) */
    std::vector<std::string> notes;  /* things worth knowing that did not fail a step */

    DiagReport();
    const DiagStep &step(Step s) const { return steps[static_cast<std::size_t>(s)]; }
};

struct DiagOptions {
    std::string resolver;                         /* "" = the system's servers (or the interface's); else this address */
    std::string iface;                            /* 0.3: the chosen network's interface ("" = Automatic: the main one) */
    std::string dns_name = "example.com";
    std::string portal_host = "connectivitycheck.gstatic.com";
    std::string portal_path = "/generate_204";
    std::vector<std::string> internet = {"1.1.1.1", "8.8.8.8"};
    int ping_timeout_ms = 1500;
    int gateway_pings = 3;
    int internet_pings = 2;                       /* per address */
    int tcp_timeout_ms = 2000;
    int dns_timeout_ms = 2000;
    int http_timeout_ms = 5000;
};

/* The network as the diagnosis uses it. Called from the diagnosis thread only. */
class DiagProbe {
public:
    virtual ~DiagProbe() = default;
    virtual NetState state() = 0;
    /* 0.3: `nmcli -g IP4.DNS,IP6.DNS device show <iface>` output; false when it could not run (no NetworkManager) */
    virtual bool nmcli_dns(const std::string &iface, std::string &text)
    {
        (void)iface;
        (void)text;
        return false;
    }
    virtual bool can_ping() = 0;
    /* one echo: true with the round trip */
    virtual bool ping(const Address &ip, int timeout_ms, const CancelToken &cancel, double *rtt_ms) = 0;
    virtual DnsResult dns(const std::string &name, std::uint16_t type, const std::string &server, int timeout_ms,
                          const CancelToken &cancel) = 0;
    virtual PortResult tcp(const Address &ip, std::uint16_t port, int timeout_ms, const CancelToken &cancel) = 0;
    virtual HttpResult http(const Address &ip, const std::string &host, const std::string &path, int timeout_ms,
                            const CancelToken &cancel) = 0;
};

/* The real network; the interface state from `source` (nullptr = the system). */
std::shared_ptr<DiagProbe> make_system_probe(std::shared_ptr<const Source> source = nullptr);

/* The state machine (the session's thread; tests call it directly with a fake probe). Publishes after every change. */
DiagReport run_diagnosis(DiagProbe &probe, const DiagOptions &opt, const CancelToken &cancel,
                         const std::function<void(const DiagReport &)> &publish);

class DiagSession {
public:
    explicit DiagSession(std::shared_ptr<DiagProbe> probe = make_system_probe());
    bool start(const DiagOptions &opt = DiagOptions()); /* LVGL thread; a running diagnosis is cancelled first */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const DiagReport> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<DiagProbe> probe_;
    Channel<DiagReport> channel_;
    Worker worker_;
};

} // namespace nt
