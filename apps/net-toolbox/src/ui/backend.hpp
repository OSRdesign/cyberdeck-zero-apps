/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: what the pages run on (no LVGL). Each tool is a Runner: start a request, stop it, poll seq() for a new
 * snapshot, take latest(). The real ones wrap the core's sessions (one worker thread each); the demonstration ones
 * (NET_TOOLBOX_DEMO, the headless scenes and the board's screenshots) answer from the core's own fakes and fixtures
 * at once, with no network and no thread, so a scene is the same every time.
 *
 *   NET_TOOLBOX_DEMO   unset: the real network. Else a list "key=value,key=value" of
 *       diag=healthy | notes | offline | dns-fail | portal | running     the diagnosis (the core's own state machine on
 *                                                                          a scripted probe)
 *       ping=mixed | unavailable | failed | stopped | lost                the ping page
 *       saved=a.example+b.example                                        saved ping targets
 *       ping_target=, dns_name=, dns_type=, dns_resolver=, port_host=, port=, port_preset=   the values the pages start with
 *       trace=healthy | silent | ratelimit | loss | unreached | unreach | starting | resolving   the traceroute chain (Start
 *             shows it as running, then Stop gives the verdict)
 *       http=ok | redirect | moved | 404 | 500 | head | plain | running | e-resolve | e-refused | e-timeout | cert-expired |
 *             cert-self | cert-mismatch | cert5 | cert20 | cert-ignored      the HTTP check, answered at once
 *       pubip=both | v4 | cgnat | offline | running        the public address page (local=<address> sets the local address)
 *       whois=domain | arin | ripe | notfound | long | offline | running        the whois page
 *       trace_target=, http_url=, http_method=GET|HEAD, http_insecure=1, whois_query=   the values those pages start with
 *       speed=latency | down-low | down-mid | down-high | up | done-a | done-b | done-c | done-d | done-f | stopped |
 *             e-network | e-tls | e-refused | e-ping       0.3: the speed test (Start shows the state, Stop gives Cancelled for
 *                                                          stopped; "idle" before Start); done-fast (0.5.0): grade A
 *                                                          on a gigabit line (1180 down, 940 up)
 *       wol=sent | waking | awake | once | noanswer | failed       0.3: what Wake now does (a machine without a host to
 *             watch always gives "sent"); wol_machines=0..20 saved machines; wol_last=name:kind+name:kind the list's last status
 *       ble=idle | scan0 | scan6 | scan25 | done25 | stopped | off | rfkill | none | bluez       0.3: the radar (Start shows the
 *             state; stopped: Stop gives Cancelled)
 *       devices=idle | scan | done | few | empty | refused    0.4: Local devices (Start shows the state; Stop gives Cancelled for
 *             scan; a board without a usable address gives the core's own refusal whatever the scenario)
 *       ports=scan | done | empty | refused      0.4: the port scan of one device (an address outside the range is refused)
 *       wifi=list | many | g24 | empty | off | nonm | vanish      0.4: Wi-Fi networks (a rescan gives the next list; vanish: the
 *             second list lacks CafeGuest); an interface that is not wlan* fails with "No Wi-Fi adapter on <if>"
 *       walk=run | long | stopped | nolink | noiw | fail      0.4: the walk test (Start shows 83 s of walking, Stop the summary)
 *       interface=wlan1      0.3: the chosen network (an interface of the fixture; one it does not have shows the "not found"
 *                            note); the diagnosis then checks that interface (the fixture's cmd/nmcli_dns_<if> is its DNS)
 *   NET_TOOLBOX_FIXTURE   a folder of tests/fixtures (deck, deck-offline, pi-3a-plus, pi-5, and the 0.3 boards with several
 *       interfaces: pi-3a-plus-dongle, pi-3a-plus-dongle-off, eth-wifi, eth-down): the interfaces come from
 *       it (also with the real tools). Demonstrations use "deck" unless it names another.
 *   DNS and port check answer by what is typed: a name starting "nx" does not exist, "slow" or "timeout" never
 *   answers; port 22 is closed, 25 gives no answer, 3389 is unreachable, the others are open.
 */
#pragma once

#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "httpcheck.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "ble.hpp"
#include "devices.hpp"
#include "speedtest.hpp"
#include "walktest.hpp"
#include "wifiscan.hpp"
#include "wol.hpp"
#include "portcheck.hpp"
#include "publicip.hpp"
#include "source.hpp"
#include "traceroute.hpp"
#include "whois.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace ui {

template <class Req, class Snap> class Runner {
public:
    virtual ~Runner() = default;
    virtual bool start(const Req &req) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual std::uint64_t seq() const = 0;
    virtual std::shared_ptr<const Snap> latest() const = 0;
};

using DiagRunner = Runner<DiagOptions, DiagReport>;
using PingRunner = Runner<PingConfig, PingSnapshot>;
using DnsRunner = Runner<DnsRequest, DnsSnapshot>;
using PortRunner = Runner<PortRequest, PortSnapshot>;
struct NoRequest {};
using TraceRunner = Runner<TraceConfig, TraceSnapshot>;
using HttpRunner = Runner<HttpCheckRequest, HttpCheckSnapshot>;
using PublicIpRunner = Runner<NoRequest, PublicIpSnapshot>;
using WhoisRunner = Runner<WhoisRequest, WhoisSnapshot>;
using SpeedRunner = Runner<SpeedConfig, SpeedSnapshot>; /* 0.3: the core sessions have their own worker threads */
using WolRunner = Runner<WolRequest, WolSnapshot>;
using BleRunner = Runner<BleConfig, BleSnapshot>;

/* 0.4 */
struct DevicesRequest {
    NetState net;
    std::string iface;     /* scope.effective() */
    std::string resolver;  /* for the reverse names */
    bool rescan = false;   /* keeps the devices already known */
};
struct PortScanRequest {
    Address address;
    bool full_range = false; /* 1-1024, else the common ports */
    ScanRange range;         /* the DevicesSnapshot's */
};
using DevicesRunner = Runner<DevicesRequest, DevicesSnapshot>;
using PortScanRunner = Runner<PortScanRequest, PortScanSnapshot>;
class WifiRunner : public Runner<WifiScanConfig, WifiSnapshot> {
public:
    virtual void rescan() = 0; /* a fresh scan now */
};
class WalkRunner : public Runner<WalkConfig, WalkSnapshot> {
public:
    virtual void reset() = 0; /* stops and forgets */
};

/* a core session as a Runner */
template <class Session, class Req, class Snap> class SessionRunner : public Runner<Req, Snap> {
public:
    template <class... Args> explicit SessionRunner(Args &&...args) : s_(std::forward<Args>(args)...) {}
    bool start(const Req &req) override { return s_.start(req); }
    void stop() override { s_.stop(); }
    bool running() const override { return s_.running(); }
    std::uint64_t seq() const override { return s_.seq(); }
    std::shared_ptr<const Snap> latest() const override { return s_.latest(); }

private:
    Session s_;
};

/* a demonstration: start() publishes make(request) at once; stop() publishes on_stop(latest) when there is one */
template <class Req, class Snap> class FixedRunner : public Runner<Req, Snap> {
public:
    using Make = std::function<Snap(const Req &)>;
    using OnStop = std::function<Snap(const Snap &)>;
    FixedRunner(Make make, OnStop on_stop = nullptr) : make_(std::move(make)), on_stop_(std::move(on_stop))
    {
        channel_.begin(0, Snap());
    }
    bool start(const Req &req) override
    {
        channel_.begin(++gen_, make_(req));
        return true;
    }
    void stop() override
    {
        if (!on_stop_) return;
        const std::shared_ptr<const Snap> now = channel_.latest();
        if (now) channel_.begin(++gen_, on_stop_(*now));
    }
    bool running() const override { return false; }
    std::uint64_t seq() const override { return channel_.seq(); }
    std::shared_ptr<const Snap> latest() const override { return channel_.latest(); }

private:
    Make make_;
    OnStop on_stop_;
    Channel<Snap> channel_;
    std::uint64_t gen_ = 0;
};

struct Backend {
    std::shared_ptr<const Source> source; /* a fixture folder, or null: the system */
    std::unique_ptr<DiagRunner> diag;
    std::unique_ptr<PingRunner> ping;
    std::unique_ptr<DnsRunner> dns;
    std::unique_ptr<PortRunner> port;
    std::unique_ptr<TraceRunner> trace;
    std::unique_ptr<HttpRunner> http;
    std::unique_ptr<PublicIpRunner> pubip;
    std::unique_ptr<WhoisRunner> whois;
    std::unique_ptr<SpeedRunner> speed;
    std::unique_ptr<WolRunner> wol;
    std::unique_ptr<BleRunner> ble;
    std::unique_ptr<DevicesRunner> devices; /* 0.4 */
    std::unique_ptr<PortScanRunner> ports;
    std::unique_ptr<WifiRunner> wifi;
    std::unique_ptr<WalkRunner> walk;
    std::function<PingAvailability()> ping_available;
    std::function<std::int64_t()> clock_ms;   /* the monotonic clock of the BLE table's times: a demonstration fixes it */
    std::function<std::time_t()> wall_clock;  /* the days left are counted from it: a demonstration fixes it */
    std::string local_ip;                     /* a demonstration's local address for the public address page ( = the board's) */
    bool demo = false;
    std::vector<std::string> saved_targets; /* a demonstration's saved ping targets */
    std::map<std::string, std::string> seeds; /* a demonstration's starting values: ping_target, dns_name, dns_type,
                                                 dns_resolver, port_host, port, port_preset, trace_target, http_url,
                                                 http_method, http_insecure, whois_query, wol_machines, wol_last */
    std::string description;                /* for the log line at start */

    /* the interfaces, gateway and resolvers now (the fixture or the system) */
    NetState read_state() const;
    /* `iw dev <iface> link` from the fixture's recorded output: true and the link when the fixture has it */
    bool fixture_link(const std::string &iface, IwLink &link) const;
    /* 0.3: the fixture's recorded `nmcli -g IP4.DNS,IP6.DNS device show <iface>` (cmd/nmcli_dns_<iface>) */
    bool fixture_nmcli(const std::string &iface, std::string &text) const;
};

/* by the environment (see the top of this file) */
Backend make_backend();

/* the demonstration backend of a spec string (tests) */
Backend make_demo_backend(const std::string &spec, std::shared_ptr<const Source> source);

} // namespace ui
} // namespace nt
