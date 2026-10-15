/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the public address (see publicip.hpp).
 */
#include "publicip.hpp"

#include "text.hpp"

#include <netinet/in.h>

namespace nt {

bool parse_cf_trace(const std::string &text, CfTrace &out)
{
    out = CfTrace();
    std::size_t lines = 0;
    for (const std::string &raw : split_lines(text)) {
        if (++lines > 64) break;
        const std::size_t eq = raw.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = trim(raw.substr(0, eq));
        std::string value = trim(raw.substr(eq + 1));
        if (value.size() > 64) value.resize(64);
        if (key == "ip") out.ip = value;
        else if (key == "loc") out.loc = value;
        else if (key == "colo") out.colo = value;
        else if (key == "http") out.http = value;
        else if (key == "tls") out.tls = value;
        else if (key == "warp") out.warp = value;
        else if (key == "sni") out.sni = value;
    }
    Address a;
    out.ok = parse_ip(out.ip, a);
    if (!out.ok) out.ip.clear();
    else out.ip = a.text();
    return out.ok;
}

std::vector<std::string> publicip_argv(int family)
{
    return {"curl", family == 6 ? "-6" : "-4", "-sS", "-m", "8", "--proto", "=https", kCfTraceUrl};
}

IpClass ip_class(const Address &a)
{
    if (a.family == AF_INET) {
        const std::uint8_t *b = a.bytes;
        if (b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168)) return IpClass::Private;
        if (b[0] == 100 && (b[1] & 0xC0) == 64) return IpClass::Cgnat;
        if (b[0] == 127) return IpClass::Loopback;
        if (b[0] == 169 && b[1] == 254) return IpClass::LinkLocal;
        if (b[0] == 0 || b[0] >= 224 || (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) ||
            (b[0] == 198 && (b[1] & 0xFE) == 18) || (b[0] == 198 && b[1] == 51 && b[2] == 100) ||
            (b[0] == 203 && b[1] == 0 && b[2] == 113))
            return IpClass::Reserved; /* this network, multicast, 240/4, documentation, benchmarks */
        return IpClass::Public;
    }
    if (a.family == AF_INET6) {
        const std::uint8_t *b = a.bytes;
        bool zero10 = true;
        for (int i = 0; i < 10; ++i)
            if (b[i]) zero10 = false;
        if (zero10 && b[10] == 0xFF && b[11] == 0xFF) { /* ::ffff:a.b.c.d */
            Address v4;
            v4.family = AF_INET;
            for (int i = 0; i < 4; ++i) v4.bytes[i] = b[12 + i];
            return ip_class(v4);
        }
        if (is_loopback(a)) return IpClass::Loopback;
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return IpClass::LinkLocal;
        if ((b[0] & 0xFE) == 0xFC) return IpClass::Private; /* unique local fc00::/7 */
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0D && b[3] == 0xB8) return IpClass::Reserved; /* documentation */
        if ((b[0] & 0xE0) == 0x20) return IpClass::Public; /* 2000::/3 */
        return IpClass::Reserved;
    }
    return IpClass::Invalid;
}

IpClass ip_class(const std::string &text)
{
    Address a;
    std::string t = trim(text);
    const std::size_t slash = t.find('/');
    if (slash != std::string::npos) t.erase(slash); /* "192.168.68.121/22" */
    return parse_ip(t, a) ? ip_class(a) : IpClass::Invalid;
}

const char *ip_class_name(IpClass c)
{
    switch (c) {
    case IpClass::Public: return "public";
    case IpClass::Private: return "private";
    case IpClass::Cgnat: return "carrier-grade NAT";
    case IpClass::Loopback: return "loopback";
    case IpClass::LinkLocal: return "link-local";
    case IpClass::Reserved: return "reserved";
    case IpClass::Invalid: return "not an address";
    }
    return "not an address";
}

NatKind nat_kind(const std::string &interface_ip, const std::string &public_ip)
{
    Address in, pub;
    std::string i = trim(interface_ip);
    const std::size_t slash = i.find('/');
    if (slash != std::string::npos) i.erase(slash);
    if (!parse_ip(i, in) || !parse_ip(trim(public_ip), pub)) return NatKind::Unknown;
    in.scope = 0;
    if (in == pub) return NatKind::Direct;
    if (ip_class(in) == IpClass::Cgnat) return NatKind::Cgnat;
    return NatKind::Nat;
}

std::string publicip_error_words(const CommandResult &r, int family)
{
    if (r.cancelled) return "cancelled";
    if (r.timed_out) return "timed out";
    if (!r.started || r.exit_code == 127) return "curl is not installed (package curl)";
    if (family == 6 && (r.exit_code == 6 || r.exit_code == 7 || r.exit_code == 28 || r.exit_code == 45))
        return "no IPv6";
    switch (r.exit_code) {
    case 0: return "an answer that is not Cloudflare's trace";
    case 6: return "the name www.cloudflare.com cannot be found (DNS)";
    case 7: return "no connection to Cloudflare";
    case 28: return "timed out";
    case 35:
    case 60: return "the secure connection to Cloudflare failed";
    default: return "curl failed (exit " + std::to_string(r.exit_code) + ")";
    }
}

PublicIpFamily parse_publicip(const CommandResult &r, int family)
{
    PublicIpFamily f;
    f.tried = true;
    if (r.ok() && parse_cf_trace(r.output, f.trace)) {
        Address a;
        parse_ip(f.trace.ip, a);
        if ((family == 6) == (a.family == AF_INET6)) {
            f.ok = true;
            f.cls = ip_class(a);
            return f;
        }
    }
    f.trace = CfTrace();
    f.error = publicip_error_words(r, family);
    return f;
}

bool PublicIpFlow::next(CommandSpec &cmd)
{
    cmd = CommandSpec();
    if (step_ > 1) return false;
    cmd.argv = publicip_argv(step_ == 0 ? 4 : 6);
    cmd.timeout_ms = 12000;
    cmd.max_output = 16 * 1024;
    return true;
}

void PublicIpFlow::feed(const CommandResult &r)
{
    if (step_ == 0) result_.v4 = parse_publicip(r, 4);
    else if (step_ == 1) result_.v6 = parse_publicip(r, 6);
    ++step_;
}

PublicIpSession::PublicIpSession() : runner_(std::make_shared<DirectRunner>()) {}

PublicIpSession::PublicIpSession(std::shared_ptr<CommandRunner> runner) : runner_(std::move(runner)) {}

bool PublicIpSession::start()
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    PublicIpSnapshot first;
    first.state = PublicIpSnapshot::State::Running;
    first.family = 4;
    channel_.begin(gen, first);
    const std::shared_ptr<CommandRunner> runner = runner_;
    const std::uint64_t started = worker_.start([this, gen, first, runner](const CancelToken &cancel) {
        PublicIpSnapshot snap = first;
        PublicIpFlow flow;
        const bool finished = run_flow(flow, *runner, cancel, [&] {
            snap.result = flow.result();
            snap.family = flow.family();
            channel_.publish(gen, snap);
        });
        snap.result = flow.result();
        snap.family = 0;
        snap.state = finished ? PublicIpSnapshot::State::Done : PublicIpSnapshot::State::Cancelled;
        channel_.publish(gen, snap);
    });
    if (!started) {
        first.state = PublicIpSnapshot::State::Done;
        first.result.v4.tried = true;
        first.result.v4.error = "could not start (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void PublicIpSession::stop()
{
    worker_.cancel();
}

} // namespace nt
