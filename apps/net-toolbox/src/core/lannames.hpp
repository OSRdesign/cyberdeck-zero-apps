/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: device names for the Local devices tool, without blocking the scan (no LVGL; tests/test_lannames.cpp
 * on synthetic packets).
 *
 * Three sources, best first:
 *   1. mDNS  a reverse PTR query to the host over UDP 5353 with the QU bit (ask for a unicast answer), the same wire
 *            format as DNS (dns.hpp builds and parses it); the answer's PTR target is the device's .local name;
 *   2. NetBIOS  an NBSTAT node-status request to UDP 137 ("*" encoded name); the answer's node-name array gives the
 *            Windows / Samba workstation name (the unique name with suffix 0x00 that is not a group name);
 *   3. reverse DNS  a PTR query to the resolver (rdns.hpp / dns_client), used only when neither of the above answered.
 * best_name() picks the first non-empty, trimmed source. A trailing ".local", ".lan", ".home" and the like are dropped
 * so the list stays short.
 *
 * The packet builders and parsers are pure and bounds-checked (the parsers reuse dns.hpp's name reader). LocalNames
 * drives the two unicast UDP sockets without blocking, like ReverseNames: want() an address, service() reads what came
 * and sends what waits, bounded and cancellable.
 */
#pragma once

#include "netaddr.hpp"
#include "sock.hpp"
#include "subnet.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {

enum class NameSource { None, Mdns, Netbios, ReverseDns };
const char *name_source_label(NameSource s); /* "", "mDNS", "NetBIOS", "reverse DNS" */

/* drop a trailing ".local", ".lan", ".home", ".fritz.box" and any trailing dots / spaces */
std::string trim_local_suffix(std::string name);

/* ---------------------------------------------------------------- mDNS (UDP 5353) */

/* a reverse PTR query for a's in-addr.arpa / ip6.arpa name, id, with the QU (unicast-response) bit set on the question
 * class. Empty on a bad address. */
std::vector<std::uint8_t> build_mdns_reverse_query(const Address &a, std::uint16_t id);
/* the first PTR target in an mDNS answer (trimmed); "" when there is none or the packet is malformed */
std::string parse_mdns_name(const std::uint8_t *buf, std::size_t len);

/* ---------------------------------------------------------------- NetBIOS (UDP 137) */

/* an NBSTAT node-status request for the wildcard name "*", id */
std::vector<std::uint8_t> build_nbstat_query(std::uint16_t id);
/* the unique workstation name (suffix 0x00, not a group name) from an NBSTAT answer; "" when none or malformed */
std::string parse_nbstat_name(const std::uint8_t *buf, std::size_t len);

/* ---------------------------------------------------------------- the priority */

struct NamePick {
    std::string name;
    NameSource source = NameSource::None;
};
/* mDNS, then NetBIOS, then reverse DNS; the first non-empty (after trimming) wins */
NamePick best_name(const std::string &mdns, const std::string &netbios, const std::string &rdns);

/* ---------------------------------------------------------------- the non-blocking driver */

class LocalNames {
public:
    static constexpr std::size_t kMaxEntries = 256;
    static constexpr int kDefaultTimeoutMs = 1500;

    explicit LocalNames(int timeout_ms = kDefaultTimeoutMs);
    /* the scan range: queries go only to its hosts (never this device); before set_range() nothing is sent */
    void set_range(const ScanRange &range) { range_ = range; }
    /* ask for a's mDNS and NetBIOS names once, unicast to that host (ignored outside the range, past kMaxEntries, for
     * a non-IPv4 address, or when the sockets could not open) */
    void want(const Address &a);
    /* never blocks: reads the answers that came, (re)sends to the addresses still waiting, drops what timed out */
    void service();
    /* the best name found so far for a ("" while none); *source: where it came from */
    std::string name(const Address &a, NameSource *source = nullptr) const;
    /* a query is still outstanding */
    bool busy() const;
    std::size_t size() const { return entries_.size(); }
    bool ready() const { return mdns_.valid() || nbns_.valid(); }

private:
    struct Entry {
        Address addr;
        std::string mdns_name, nbns_name;
        std::uint16_t id = 0;
        std::int64_t sent_ms = 0;
        bool sent = false;
        bool done = false;
    };
    void send(Entry &e);
    void absorb_mdns(const std::uint8_t *buf, std::size_t len, const Address &from);
    void absorb_nbns(const std::uint8_t *buf, std::size_t len, const Address &from);
    ScanRange range_;
    Fd mdns_, nbns_;
    int timeout_ms_;
    std::uint16_t next_id_;
    std::vector<Entry> entries_;
};

} // namespace nt
