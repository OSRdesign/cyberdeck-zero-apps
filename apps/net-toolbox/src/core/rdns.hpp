/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: reverse names for the traceroute hops without blocking the probing (no LVGL; tests/test_trace.cpp
 * against the fake DNS server on loopback).
 *
 * One UDP socket to the resolver; a PTR query (dns.hpp's builder and parser, the in-addr.arpa / ip6.arpa name of
 * netaddr.hpp) is sent when an address is first seen, at most kMaxPending at a time; service() reads the answers that
 * are there without waiting and gives up a query after its timeout (one try). Bounded: at most kMaxEntries addresses,
 * each looked up once (the cache), names cut at 253 bytes. Everything runs on the traceroute's thread.
 */
#pragma once

#include "netaddr.hpp"
#include "sock.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

class ReverseNames {
public:
    static constexpr std::size_t kMaxEntries = 64;
    static constexpr std::size_t kMaxPending = 4;
    /* 0.4 Local devices: a whole /24 (254 hosts), more questions in flight so a silent resolver stays within budget */
    static constexpr std::size_t kMaxEntriesLan = 256;
    static constexpr std::size_t kMaxPendingLan = 16;

    /* server: an IP literal ("" = no lookups); the defaults are the traceroute's */
    explicit ReverseNames(const std::string &server, std::uint16_t port = 53, int timeout_ms = 1500,
                          std::size_t max_entries = kMaxEntries, std::size_t max_pending = kMaxPending);
    /* asks for a's name once (ignored when the cache is full or there is no server) */
    void want(const Address &a);
    /* never blocks: reads the answers that came, sends what waits, drops what timed out */
    void service();
    /* the name ("" while unknown, or when there is none); *done: the lookup is over */
    std::string name(const Address &a, bool *done = nullptr) const;
    /* a lookup is still waiting */
    bool busy() const;
    std::size_t size() const { return entries_.size(); }
    /* counts the lookups that ended (a name or none): something to show changed */
    std::uint64_t changes() const { return changes_; }

private:
    enum class State { Queued, Sent, Done };
    struct Entry {
        Address addr;
        State state = State::Queued;
        std::string qname;
        std::uint16_t id = 0;
        std::int64_t sent_ms = 0;
        std::string name;
    };
    void send(Entry &e);
    Fd fd_;
    int timeout_ms_;
    std::size_t max_entries_;
    std::size_t max_pending_;
    std::uint16_t next_id_;
    std::uint64_t changes_ = 0;
    std::vector<Entry> entries_;
};

} // namespace nt
