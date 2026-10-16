/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the Local devices tool (no LVGL; tests/test_devices.cpp, tests/test_devscan.cpp).
 *
 * A local IP scanner, no root, only ever the chosen interface's own private /24 (subnet.hpp). Two parts:
 *
 *   Discovery (DevicesSession, run_devices)
 *     The subnet is swept with unprivileged ICMP echo (ping.cpp's datagram socket) and cheap non-blocking TCP connect
 *     probes to a few ports, which makes the kernel ARP-resolve every live host; /proc/net/arp then names the MAC of
 *     the hosts that are there (complete entries only, flag 0x2). A host's name is looked up best-first: mDNS, then
 *     NetBIOS, then reverse DNS (lannames.hpp); its vendor from the OUI table (oui.hpp); a locally-administered
 *     (random) MAC is shown as a private address with no vendor. This device and the default gateway are marked.
 *     Devices appear in the snapshot as they are found (a live list). Rescan keeps the devices already known and
 *     refreshes them. Everything is bounded (sockets, per-host timeouts, a total budget of about 10 s for a /24) and
 *     cancellable within a poll slice.
 *
 *   Port scan of one device (PortScanSession, run_port_scan)
 *     A TCP connect scan of one private address on the chosen interface: common ports (a curated list with service
 *     names) or 1-1024. Non-blocking, bounded concurrency (~64), a per-connect timeout of about 700 ms for the LAN,
 *     progress counters and the open list with a service name; cancellable. Never the gateway's far side, never a
 *     public address: the engine itself refuses any target that is not a private / link-local host of the scanned
 *     subnet (scan_target_problem: also 0.0.0.0, loopback, multicast, broadcast) before any socket is made.
 *
 *   Where the probes go: the discovery's Prober and NameLookup base classes refuse any address outside the scan range
 *   (and this device), so the ICMP / TCP pokes and the unicast mDNS / NetBIOS queries reach only hosts of the range.
 *
 * Snapshots are the pages worker's whole input: DevicesSnapshot and PortScanSnapshot, polled through seq()/latest() as
 * every other tool. The engines (run_*) take injectable probing so the tests drive them on loopback fakes.
 */
#pragma once

#include "lannames.hpp"
#include "netaddr.hpp"
#include "ping.hpp"
#include "subnet.hpp"
#include "worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {

/* ---------------------------------------------------------------- one device */

struct Device {
    std::string ip;              /* "192.168.68.1" */
    std::uint32_t ip_order = 0;  /* host byte order, for sorting */
    std::string mac;             /* lower-case "aa:bb:cc:dd:ee:ff"; "" when not known (this device, or no ARP yet) */
    std::string vendor;          /* from the OUI table; "" when unknown or a private MAC */
    bool vendor_private = false; /* the MAC is locally administered (random): shown as a private address, no vendor */
    std::string name;            /* best name found, "" when none */
    NameSource name_source = NameSource::None;
    bool has_rtt = false;
    double rtt_ms = 0;           /* the ICMP round trip when the host answered a ping */
    bool is_gateway = false;
    bool is_self = false;
};

/* ---------------------------------------------------------------- discovery snapshot */

struct DevicesSnapshot {
    enum class State { Idle, Scanning, Done, Cancelled, Failed };
    State state = State::Idle;
    std::string error;           /* Failed: plain words (subnet_problem and the like) */
    std::string interface;       /* the interface scanned */
    std::string subnet;          /* "192.168.68.0/24" */
    std::string gateway;         /* the default gateway address, "" when none */
    ScanRange range;             /* the range scanned: hand it to PortScanSession::start for a device of this list */
    int total = 0;               /* addresses to probe */
    int probed = 0;              /* addresses poked so far (progress) */
    int found = 0;               /* devices found (including this device) */
    bool naming = false;         /* still resolving names */
    std::vector<Device> devices; /* sorted by IP numeric; appear as found */
    bool running() const { return state == State::Scanning; }
};

/* ---------------------------------------------------------------- port-scan snapshot */

struct OpenPort {
    std::uint16_t port = 0;
    std::string service;         /* "https", "ssh", ... "" when unknown */
    int connect_ms = 0;
};

struct PortScanSnapshot {
    enum class State { Idle, Scanning, Done, Cancelled, Failed };
    State state = State::Idle;
    std::string error;
    std::string address;         /* the device scanned */
    bool full_range = false;     /* true = 1-1024, false = the common ports */
    int total = 0;
    int scanned = 0;
    int open_count = 0;
    std::vector<OpenPort> open;  /* sorted by port, appear as found */
    bool running() const { return state == State::Scanning; }
};

/* a curated list of common TCP ports with their usual service names (extends port_presets with LAN services) */
const std::vector<std::pair<std::uint16_t, const char *>> &local_common_ports();
/* the service name of a port (port_service() extended with the common-port list); "" when unknown */
const char *local_port_service(std::uint16_t port);

/* ---------------------------------------------------------------- injectable probing (for the tests) */

/* Pokes one address so the kernel ARP-resolves it (ICMP echo + a few TCP connects); never blocks.
 * The guard is in this base class, so every prober (the real one and the fakes) obeys it: poke() reaches only an address
 * inside the range given to begin(), never this device; before begin() (or with a range that is not ok) nothing is sent
 * (fail closed). A refused poke returns false and is counted. */
class Prober {
public:
    virtual ~Prober() = default;
    void begin(const ScanRange &range)
    {
        range_ = range;
        on_begin(range);
    }
    bool poke(std::uint32_t host_order)
    {
        if (!range_.contains(host_order) || host_order == range_.self) {
            ++refused_;
            return false;
        }
        probe(host_order);
        return true;
    }
    int refused() const { return refused_; }
    virtual void service() {}                 /* read any ICMP replies that arrived */
    virtual double rtt_ms(std::uint32_t host_order) const { (void)host_order; return -1; } /* -1 = unknown */

protected:
    virtual void on_begin(const ScanRange &range) { (void)range; }
    virtual void probe(std::uint32_t host_order) = 0; /* only ever called for an address inside the range */

private:
    ScanRange range_;
    int refused_ = 0;
};

/* Reads the neighbour table (the text of /proc/net/arp). */
class NeighbourReader {
public:
    virtual ~NeighbourReader() = default;
    virtual std::string read() = 0;
};

/* Best-first name for an address (mDNS / NetBIOS / reverse DNS); optional (nullptr = no names). Same guard as Prober:
 * want() asks only for an IPv4 address inside the range given to begin(), never this device (the mDNS and NetBIOS
 * queries go unicast to that host only; no multicast is sent); before begin() nothing is asked. */
class NameLookup {
public:
    virtual ~NameLookup() = default;
    void begin(const ScanRange &range)
    {
        range_ = range;
        on_begin(range);
    }
    bool want(const Address &a)
    {
        const std::uint32_t h = ipv4_to_host_order(a);
        if (a.family != AF_INET || !range_.contains(h) || h == range_.self) return false;
        ask(a);
        return true;
    }
    virtual void service() = 0;
    virtual std::string name(const Address &a, NameSource *source) const = 0;
    virtual bool busy() const = 0;

protected:
    virtual void on_begin(const ScanRange &range) { (void)range; }
    virtual void ask(const Address &a) = 0; /* only ever called for an address inside the range */

private:
    ScanRange range_;
};

/* the real ones (the session makes a prober and a name lookup per run, on the worker thread) */
std::unique_ptr<Prober> make_icmp_tcp_prober();      /* one ICMP socket + TCP connect probes, bound to the active iface */
std::unique_ptr<NeighbourReader> make_proc_arp_reader(); /* /proc/net/arp (NET_TOOLBOX_ARP overrides the path) */
std::unique_ptr<NameLookup> make_name_lookup(const std::string &resolver); /* mDNS + NetBIOS + reverse DNS */

/* ---------------------------------------------------------------- the discovery engine */

struct DevicesConfig {
    ScanRange range;
    std::string self_mac;        /* the interface's MAC (lower case), "" when unknown */
    std::string gateway;         /* the gateway IPv4 text, "" when none */
    std::vector<Device> keep;    /* rescan: previously known devices to keep and refresh */
    int wave = 24;               /* poke this many addresses between ARP reads */
    int settle_ms = 400;         /* wait between ARP reads after the sweep */
    int total_budget_ms = 12000; /* the whole run stops after this (a /24 in about 10 s on the Zero 2 W) */
    int rounds = 5;              /* ARP read / name service rounds after the sweep */
};

DevicesSnapshot run_devices(const DevicesConfig &cfg, Prober &prober, NeighbourReader &arp, NameLookup *names,
                            const CancelToken &cancel, const std::function<void(const DevicesSnapshot &)> &publish);

class DevicesSession {
public:
    DevicesSession();                                 /* the real prober, ARP reader and name lookup (made per run) */
    DevicesSession(std::shared_ptr<Prober> prober, std::shared_ptr<NeighbourReader> arp,
                   std::shared_ptr<NameLookup> names); /* tests: shared by the runs, which must not overlap */
    /* LVGL thread; a running scan is stopped first. net = read_net_state() of the system, iface = the chosen
     * interface (scope.effective()), resolver = the DNS server for reverse names. rescan keeps the known devices. */
    bool start(const NetState &net, const std::string &iface, const std::string &resolver, bool rescan = false);
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const DevicesSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */

private:
    std::shared_ptr<Prober> prober_;          /* injected (tests); nullptr with real_ */
    std::shared_ptr<NeighbourReader> arp_;
    std::shared_ptr<NameLookup> names_;       /* injected (tests); nullptr = no names, or made per run with real_ */
    bool real_ = false;
    Channel<DevicesSnapshot> channel_;
    Worker worker_;
};

/* ---------------------------------------------------------------- the port-scan engine */

struct PortScanConfig {
    Address address;
    ScanRange range;                /* the scanned subnet: when ok, the address must lie inside it */
    bool full_range = false;        /* 1-1024, else the common ports */
    int timeout_ms = 700;           /* per connect, LAN */
    int concurrency = 64;           /* sockets in flight at once */
    std::vector<std::uint16_t> ports; /* tests: an explicit port list (loopback listeners); empty = full_range/common */
    bool allow_loopback = false;    /* tests only, off by default: a 127.x listener may be scanned */
};

/* The target is checked first, in the engine itself (scan_target_problem): anything but a private / link-local IPv4
 * host (inside cfg.range when it is ok) ends at once as Failed with plain words, before any socket is made. */
PortScanSnapshot run_port_scan(const PortScanConfig &cfg, const CancelToken &cancel,
                               const std::function<void(const PortScanSnapshot &)> &publish);

class PortScanSession {
public:
    /* LVGL thread; a running scan is stopped first. range = the DevicesSnapshot's range: it is required (a range that is
     * not ok refuses with "No network") and the address must be a host inside it; refused with words otherwise. */
    bool start(const Address &address, bool full_range, const ScanRange &range);
    void stop();
    bool running() const { return worker_.running(); }
    std::uint64_t seq() const { return channel_.seq(); }
    std::shared_ptr<const PortScanSnapshot> latest() const { return channel_.latest(); }
    bool wait(int timeout_ms) const { return worker_.wait(timeout_ms); } /* tests */
    void allow_loopback_for_tests(bool on) { allow_loopback_ = on; } /* tests only, off by default */

private:
    bool allow_loopback_ = false;
    Channel<PortScanSnapshot> channel_;
    Worker worker_;
};

/* the subnet text "192.168.68.0/24" of a range (for the snapshot) */
std::string subnet_text(const ScanRange &range);

} // namespace nt
