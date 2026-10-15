/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: our own DNS client (no LVGL; tests/test_dns.cpp runs it against a fake server on loopback).
 *
 * Why not getaddrinfo: it cannot be cancelled, it hides which server answered, and the resolver must be one the user
 * picks. A query goes over UDP to one server (EDNS0, 1232 bytes), is sent again after each try's timeout, and is
 * asked again over TCP when the answer comes back truncated (TC). A server that rejects EDNS (FORMERR, NOTIMP) is
 * asked once more without it. Every wait is a poll() slice, so a cancel ends a query within kSliceMs.
 *
 * Host names for ping and port check go through resolve_host(): a literal address is used as is; /etc/hosts first
 * (localhost and the board's own name); then A (IPv4 first), then AAAA.
 */
#pragma once

#include "dns.hpp"
#include "netaddr.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

/* ---------------------------------------------------------------- resolvers */

struct ResolverPreset {
    const char *label;   /* "System", "Cloudflare", "Google", "Quad9" */
    const char *address; /* "" = the system default (resolv.conf's first server) */
};

/* System, 1.1.1.1, 8.8.8.8, 9.9.9.9 */
const std::vector<ResolverPreset> &resolver_presets();

/* The server to ask: "" or "system" -> the first system server ("" when there is none); an IP -> itself; anything
 * else -> "". */
std::string pick_resolver(const std::string &choice, const std::vector<std::string> &system_servers);

/* ---------------------------------------------------------------- one query */

struct DnsQuery {
    std::string name;            /* the name to ask (make_question() turns an address into its PTR name) */
    std::uint16_t type = dns::kA;
    std::string server;          /* an IP literal */
    std::uint16_t port = 53;
    int timeout_ms = 2000;       /* per try */
    int tries = 2;
    bool tcp_fallback = true;
    bool edns = true;
};

struct DnsResult {
    std::string name;            /* the name asked */
    std::uint16_t type = 0;
    std::string server;          /* the resolver used */
    bool answered = false;       /* a response arrived, whatever its code */
    std::string error;           /* when not answered: "timed out", "connection refused", "cancelled", ... */
    int rcode = -1;
    std::string rcode_name;      /* "NOERROR", "NXDOMAIN", ... */
    std::string rcode_words;     /* "OK", "no such name", ... */
    bool via_tcp = false;        /* the UDP answer was truncated: this one came over TCP */
    bool authoritative = false;
    int elapsed_ms = 0;          /* first send to the answer (or to the give-up) */
    int tries_used = 0;
    std::vector<dns::Record> answers;
    std::vector<dns::Record> authority;
    std::vector<std::string> chain; /* the CNAME chain from the name (just the name when there is none) */

    bool ok() const { return answered && rcode == dns::kNoError; }
    /* the A / AAAA data of the answer ("" when none) */
    std::vector<std::string> addresses(std::uint16_t want) const;
};

/* What the user typed, ready to ask: trimmed, a trailing dot dropped; an address becomes its in-addr.arpa / ip6.arpa
 * name and the type PTR (whatever type was chosen). False with words in *why for an empty or invalid name. */
bool make_question(const std::string &input, std::uint16_t &type, std::string &qname, std::string *why = nullptr);

/* Blocking (a worker thread): the whole exchange with one server. Never throws. */
DnsResult dns_query(const DnsQuery &q, const CancelToken &cancel);

/* The same question to several servers, one after the other; each result is reported as it comes. */
std::vector<DnsResult> dns_compare(const DnsQuery &base, const std::vector<std::string> &servers,
                                   const CancelToken &cancel,
                                   const std::function<void(const DnsResult &)> &each = nullptr);

/* ---------------------------------------------------------------- host names for the other tools */

/* /etc/hosts: the addresses listed for a name (case-insensitive), IPv4 ones first */
std::vector<Address> hosts_lookup(const std::string &hosts_text, const std::string &name);

struct ResolveOptions {
    std::string server;          /* the resolver ("" = none: only literals and /etc/hosts) */
    std::uint16_t port = 53;
    int timeout_ms = 2000;
    int tries = 2;
    std::string hosts_path = "/etc/hosts"; /* "" = skip */
};

struct Resolved {
    bool ok = false;
    Address address;             /* the one to use: the first IPv4 address, else the first IPv6 one */
    std::vector<Address> all;
    bool literal = false;        /* the host was an address */
    bool from_hosts = false;
    std::string server;          /* the resolver that answered */
    int elapsed_ms = 0;
    std::string error;           /* plain words: "example.invalid does not exist (NXDOMAIN)", ... */
};

Resolved resolve_host(const std::string &host, const ResolveOptions &opt, const CancelToken &cancel);

/* ---------------------------------------------------------------- the DNS lookup tool */

struct DnsRequest {
    std::string input;                 /* what the user typed */
    std::uint16_t type = dns::kA;
    std::vector<std::string> servers;  /* one: a lookup; several: compare (one after the other) */
    std::uint16_t port = 53;
    int timeout_ms = 2000;
    int tries = 2;
};

struct DnsSnapshot {
    enum class State { Idle, Running, Done, Failed, Cancelled };
    State state = State::Idle;
    std::string question;               /* the name asked (the PTR name for an address) */
    std::uint16_t type = 0;
    std::string error;                  /* Failed: why nothing was asked */
    std::vector<std::string> servers;   /* in the order they are asked */
    std::vector<DnsResult> results;     /* one per server done so far */
    bool compare() const { return servers.size() > 1; }
};

class DnsSession {
public:
    DnsSession() = default;
    /* LVGL thread. A running lookup is cancelled first. False (and a Failed snapshot) for a bad name or no server. */
    bool start(const DnsRequest &req);
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const DnsSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    Channel<DnsSnapshot> channel_;
    Worker worker_;
};

} // namespace nt
