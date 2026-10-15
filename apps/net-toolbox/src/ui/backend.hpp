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
 *   NET_TOOLBOX_FIXTURE   a folder of tests/fixtures (deck, deck-offline, pi-3a-plus, pi-5): the interfaces come from
 *       it (also with the real tools). Demonstrations use "deck" unless it names another.
 *   DNS and port check answer by what is typed: a name starting "nx" does not exist, "slow" or "timeout" never
 *   answers; port 22 is closed, 25 gives no answer, 3389 is unreachable, the others are open.
 */
#pragma once

#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"
#include "source.hpp"

#include <cstdint>
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
    std::function<PingAvailability()> ping_available;
    bool demo = false;
    std::vector<std::string> saved_targets; /* a demonstration's saved ping targets */
    std::map<std::string, std::string> seeds; /* a demonstration's starting values: ping_target, dns_name, dns_type,
                                                 dns_resolver, port_host, port, port_preset */
    std::string description;                /* for the log line at start */

    /* the interfaces, gateway and resolvers now (the fixture or the system) */
    NetState read_state() const;
    /* `iw dev <iface> link` from the fixture's recorded output: true and the link when the fixture has it */
    bool fixture_link(const std::string &iface, IwLink &link) const;
};

/* by the environment (see the top of this file) */
Backend make_backend();

/* the demonstration backend of a spec string (tests) */
Backend make_demo_backend(const std::string &spec, std::shared_ptr<const Source> source);

} // namespace ui
} // namespace nt
