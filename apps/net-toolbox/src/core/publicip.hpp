/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the public address, as the internet sees it (no LVGL; tests/test_publicip.cpp).
 *
 * Two requests with the system's curl, one per family, never through a shell:
 *   curl -4 -sS -m 8 --proto =https https://www.cloudflare.com/cdn-cgi/trace
 *   curl -6 -sS -m 8 --proto =https https://www.cloudflare.com/cdn-cgi/trace
 * Cloudflare answers key=value lines: ip (the public address), loc (the country, ISO 3166 two letters), colo (the
 * Cloudflare site, an airport code), http, tls, warp. An IPv6 request that fails is normal (no IPv6 on most home
 * networks): it is reported as "no IPv6", not as an error.
 *
 * ip_class() tells private, carrier-grade NAT (100.64.0.0/10), public, ... so the page can say "you are behind
 * carrier-grade NAT" (nat_kind()).
 */
#pragma once

#include "cmdrun.hpp"
#include "netaddr.hpp"
#include "worker.hpp"

#include <memory>
#include <string>
#include <vector>

namespace nt {

struct CfTrace {
    bool ok = false;           /* an `ip=` line with an address */
    std::string ip, loc, colo, http, tls, warp, sni;
};

/* key=value lines; unknown keys ignored, values trimmed and bounded */
bool parse_cf_trace(const std::string &text, CfTrace &out);
/* family 4 or 6 */
std::vector<std::string> publicip_argv(int family);
constexpr const char *kCfTraceUrl = "https://www.cloudflare.com/cdn-cgi/trace";

enum class IpClass { Invalid, Public, Private, Cgnat, Loopback, LinkLocal, Reserved };
IpClass ip_class(const Address &a);
IpClass ip_class(const std::string &text);
/* "public", "private", "carrier-grade NAT", "loopback", "link-local", "reserved", "not an address" */
const char *ip_class_name(IpClass c);

enum class NatKind {
    Unknown,   /* an address is missing */
    Direct,    /* the interface has the public address itself */
    Nat,       /* a private interface address: the usual home router */
    Cgnat      /* the interface address is in 100.64.0.0/10: the provider shares one public address */
};
NatKind nat_kind(const std::string &interface_ip, const std::string &public_ip);

struct PublicIpFamily {
    bool tried = false;
    bool ok = false;
    CfTrace trace;
    IpClass cls = IpClass::Invalid;
    std::string error;         /* plain words when not ok: "no IPv6", "timed out", ... */
};

struct PublicIpResult {
    PublicIpFamily v4, v6;
};

/* the words for a failed request of that family */
std::string publicip_error_words(const CommandResult &r, int family);
/* one request's result */
PublicIpFamily parse_publicip(const CommandResult &r, int family);

/* IPv4, then IPv6. */
class PublicIpFlow : public CommandFlow {
public:
    bool next(CommandSpec &cmd) override;
    void feed(const CommandResult &r) override;
    const PublicIpResult &result() const { return result_; }
    int family() const { return step_ == 0 ? 4 : step_ == 1 ? 6 : 0; } /* the step in progress (0: done) */

private:
    int step_ = 0;
    PublicIpResult result_;
};

struct PublicIpSnapshot {
    enum class State { Idle, Running, Done, Cancelled };
    State state = State::Idle;
    int family = 0;            /* Running: 4 or 6 */
    PublicIpResult result;
    bool running() const { return state == State::Running; }
};

class PublicIpSession {
public:
    PublicIpSession();         /* DirectRunner */
    explicit PublicIpSession(std::shared_ptr<CommandRunner> runner);
    bool start();              /* LVGL thread */
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const PublicIpSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<CommandRunner> runner_;
    Channel<PublicIpSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
