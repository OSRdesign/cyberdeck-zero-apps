/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the Local devices discovery engine, the real prober / ARP reader / name lookup and the session
 * (see devices.hpp). The port scan is in devscan.cpp.
 */
#include "devices.hpp"

#include "oui.hpp"
#include "rdns.hpp"
#include "scope.hpp"
#include "sock.hpp"
#include "text.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nt {

std::string subnet_text(const ScanRange &range)
{
    if (!range.ok) return std::string();
    const std::uint32_t mask = range.prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - range.prefix));
    return ipv4_text(range.self & mask) + "/" + std::to_string(range.prefix);
}

/* ---------------------------------------------------------------- the real prober */

namespace {

std::uint16_t icmp_checksum(const std::uint8_t *p, std::size_t n)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) sum += static_cast<std::uint32_t>((p[i] << 8) | p[i + 1]);
    if (n & 1) sum += static_cast<std::uint32_t>(p[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum & 0xFFFF);
}

/* TCP ports a connect SYN is sent to, to make the kernel ARP a host that drops ICMP */
const std::uint16_t kPokePorts[] = {80, 443, 22, 445, 62078, 8080};

class IcmpTcpProber : public Prober {
public:
    IcmpTcpProber()
    {
        if (getrandom(cookie_, sizeof(cookie_), GRND_NONBLOCK) != static_cast<ssize_t>(sizeof(cookie_))) {
            const std::int64_t t = now_us() ^ (static_cast<std::int64_t>(getpid()) << 20);
            std::memcpy(cookie_, &t, sizeof(cookie_));
        }
        Fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP));
        if (fd.valid() && bind_to_active_device(fd.get()) == 0) icmp_ = std::move(fd);
    }
    ~IcmpTcpProber() override { tcp_.clear(); }

    void service() override
    {
        if (!icmp_.valid()) return;
        std::uint8_t buf[256];
        for (;;) {
            const ssize_t n = recv(icmp_.get(), buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n < 16) continue;
            if (buf[0] != 0) continue; /* echo reply */
            if (std::memcmp(buf + 8, cookie_, sizeof(cookie_)) != 0) continue;
            const std::uint16_t seq = static_cast<std::uint16_t>((buf[6] << 8) | buf[7]);
            const auto it = by_seq_.find(seq);
            if (it == by_seq_.end()) continue;
            const std::int64_t sent = it->second.second;
            rtt_[it->second.first] = static_cast<double>(now_us() - sent) / 1000.0;
        }
    }

    double rtt_ms(std::uint32_t host) const override
    {
        const auto it = rtt_.find(host);
        return it == rtt_.end() ? -1.0 : it->second;
    }

protected:
    /* only reached through Prober::poke(), which has checked that host is inside the scan range */
    void probe(std::uint32_t host) override
    {
        send_icmp(host);
        for (const std::uint16_t port : kPokePorts) send_syn(host, port);
    }

private:
    void send_icmp(std::uint32_t host)
    {
        if (!icmp_.valid()) return;
        const std::uint16_t seq = next_seq_++;
        by_seq_[seq] = {host, now_us()};
        std::uint8_t pkt[16] = {};
        pkt[0] = 8; /* echo request */
        pkt[6] = static_cast<std::uint8_t>(seq >> 8);
        pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
        std::memcpy(pkt + 8, cookie_, sizeof(cookie_));
        const std::uint16_t c = icmp_checksum(pkt, sizeof(pkt));
        pkt[2] = static_cast<std::uint8_t>(c >> 8);
        pkt[3] = static_cast<std::uint8_t>(c & 0xFF);
        const Address a = host_order_to_ipv4(host);
        sockaddr_storage ss;
        const socklen_t sl = a.to_sockaddr(0, ss);
        if (sl) {
            const ssize_t n = sendto(icmp_.get(), pkt, sizeof(pkt), MSG_NOSIGNAL | MSG_DONTWAIT,
                                     reinterpret_cast<const sockaddr *>(&ss), sl);
            (void)n;
        }
    }

    void send_syn(std::uint32_t host, std::uint16_t port)
    {
        Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
        if (!fd.valid()) return;
        if (bind_to_active_device(fd.get()) != 0) return;
        const Address a = host_order_to_ipv4(host);
        sockaddr_storage ss;
        const socklen_t sl = a.to_sockaddr(port, ss);
        if (!sl) return;
        /* non-blocking connect: the SYN (and the ARP it needs) leaves at once; the result is of no interest */
        const int rc = connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sl);
        (void)rc;
        tcp_.push_back(std::move(fd));
        if (tcp_.size() > 64) tcp_.erase(tcp_.begin()); /* bounded sockets: the oldest SYN is long gone */
    }

    Fd icmp_;
    std::vector<Fd> tcp_;
    std::uint8_t cookie_[8] = {};
    std::uint16_t next_seq_ = 1;
    std::unordered_map<std::uint16_t, std::pair<std::uint32_t, std::int64_t>> by_seq_; /* seq -> {host, sent_us} */
    std::unordered_map<std::uint32_t, double> rtt_;
};

class ProcArpReader : public NeighbourReader {
public:
    std::string read() override
    {
        const char *over = std::getenv("NET_TOOLBOX_ARP");
        const std::string path = over && over[0] ? over : "/proc/net/arp";
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::string();
        std::string out;
        char buf[8192];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0 && out.size() < 262144) out.append(buf, static_cast<std::size_t>(n));
        close(fd);
        return out;
    }
};

class CombinedNames : public NameLookup {
public:
    explicit CombinedNames(const std::string &resolver)
        : rdns_(resolver, 53, 1200, ReverseNames::kMaxEntriesLan, ReverseNames::kMaxPendingLan)
    {
    }
    void service() override
    {
        local_.service();
        rdns_.service();
    }
    std::string name(const Address &a, NameSource *source) const override
    {
        NameSource ls = NameSource::None;
        const std::string local = local_.name(a, &ls);
        if (!local.empty()) {
            if (source) *source = ls;
            return local;
        }
        const std::string r = trim_local_suffix(rdns_.name(a));
        if (source) *source = r.empty() ? NameSource::None : NameSource::ReverseDns;
        return r;
    }
    bool busy() const override { return local_.busy() || rdns_.busy(); }

protected:
    void on_begin(const ScanRange &range) override { local_.set_range(range); }
    /* only reached through NameLookup::want(), which has checked that a is a host of the scan range */
    void ask(const Address &a) override
    {
        local_.want(a);  /* unicast mDNS + NetBIOS to that host only */
        rdns_.want(a);   /* a PTR question to the resolver about it */
    }

private:
    LocalNames local_;
    ReverseNames rdns_;
};

/* ---------------------------------------------------------------- the device list */

Device *find_device(std::vector<Device> &v, std::uint32_t ip_order)
{
    for (Device &d : v)
        if (d.ip_order == ip_order) return &d;
    return nullptr;
}

void apply_vendor(Device &d)
{
    if (d.mac.empty()) {
        d.vendor.clear();
        d.vendor_private = false;
        return;
    }
    const VendorLookup v = lookup_vendor(oui_table(), d.mac);
    d.vendor_private = v.private_address;
    d.vendor = v.vendor;
}

void sort_devices(std::vector<Device> &v)
{
    std::sort(v.begin(), v.end(), [](const Device &a, const Device &b) { return a.ip_order < b.ip_order; });
}

} // namespace

std::unique_ptr<Prober> make_icmp_tcp_prober() { return std::make_unique<IcmpTcpProber>(); }
std::unique_ptr<NeighbourReader> make_proc_arp_reader() { return std::make_unique<ProcArpReader>(); }
std::unique_ptr<NameLookup> make_name_lookup(const std::string &resolver)
{
    return std::make_unique<CombinedNames>(resolver);
}

/* ---------------------------------------------------------------- the engine */

DevicesSnapshot run_devices(const DevicesConfig &cfg, Prober &prober, NeighbourReader &arp, NameLookup *names,
                            const CancelToken &cancel, const std::function<void(const DevicesSnapshot &)> &publish)
{
    DevicesSnapshot snap;
    snap.interface = cfg.range.iface;
    snap.subnet = subnet_text(cfg.range);
    snap.gateway = cfg.gateway;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    if (!cfg.range.ok) {
        snap.state = DevicesSnapshot::State::Failed;
        snap.error = cfg.range.problem.empty() ? "No network" : cfg.range.problem;
        emit();
        return snap;
    }
    snap.state = DevicesSnapshot::State::Scanning;
    snap.total = cfg.range.count();
    snap.range = cfg.range;

    /* the range first: the prober and the name lookup refuse every address outside it */
    prober.begin(cfg.range);
    if (names) names->begin(cfg.range);

    std::uint32_t gw_order = 0;
    const bool has_gw = !cfg.gateway.empty() && [&] {
        Address a;
        if (!parse_ip(cfg.gateway, a) || a.family != AF_INET) return false;
        gw_order = ipv4_to_host_order(a);
        return true;
    }();

    /* seed: the devices kept from a previous scan (rescan), then this device and the gateway */
    for (const Device &k : cfg.keep)
        if (cfg.range.contains(k.ip_order) && !find_device(snap.devices, k.ip_order)) snap.devices.push_back(k);
    if (!find_device(snap.devices, cfg.range.self)) {
        Device self;
        self.ip_order = cfg.range.self;
        self.ip = ipv4_text(self.ip_order);
        self.mac = cfg.self_mac;
        self.is_self = true;
        apply_vendor(self);
        snap.devices.push_back(self);
    }
    if (has_gw && cfg.range.contains(gw_order) && !find_device(snap.devices, gw_order)) {
        Device gw;
        gw.ip_order = gw_order;
        gw.ip = ipv4_text(gw_order);
        gw.is_gateway = true;
        snap.devices.push_back(gw);
    }
    for (Device &d : snap.devices) {
        d.is_gateway = has_gw && d.ip_order == gw_order;
        if (d.ip_order == cfg.range.self) d.is_self = true;
        if (names && !d.is_self) names->want(host_order_to_ipv4(d.ip_order));
    }
    snap.found = static_cast<int>(snap.devices.size());
    sort_devices(snap.devices);
    emit();

    const std::int64_t deadline = now_ms() + std::max(1000, cfg.total_budget_ms);

    /* merge the complete ARP neighbours of the scanned interface into the list */
    const auto merge_arp = [&]() -> bool {
        bool changed = false;
        for (const ArpEntry &e : arp_neighbours(arp.read(), cfg.range.iface)) {
            Address a;
            if (!parse_ip(e.ip, a) || a.family != AF_INET) continue;
            const std::uint32_t ho = ipv4_to_host_order(a);
            if (!cfg.range.contains(ho)) continue;
            Device *d = find_device(snap.devices, ho);
            if (!d) {
                Device nd;
                nd.ip_order = ho;
                nd.ip = e.ip;
                snap.devices.push_back(nd);
                d = &snap.devices.back();
                changed = true;
                if (names) names->want(a);
            }
            if (d->mac != e.mac) {
                d->mac = e.mac;
                apply_vendor(*d);
                changed = true;
            }
            if (has_gw && ho == gw_order && !d->is_gateway) {
                d->is_gateway = true;
                changed = true;
            }
        }
        return changed;
    };

    /* record the ICMP round trips the prober measured */
    const auto merge_rtt = [&]() -> bool {
        bool changed = false;
        for (Device &d : snap.devices) {
            const double rtt = prober.rtt_ms(d.ip_order);
            if (rtt >= 0 && (!d.has_rtt || d.rtt_ms != rtt)) {
                d.has_rtt = true;
                d.rtt_ms = rtt;
                changed = true;
            }
        }
        return changed;
    };

    /* update names from the lookup */
    const auto merge_names = [&]() -> bool {
        if (!names) return false;
        bool changed = false;
        for (Device &d : snap.devices) {
            if (d.is_self) continue;
            NameSource src = NameSource::None;
            const std::string n = names->name(host_order_to_ipv4(d.ip_order), &src);
            if (!n.empty() && (d.name != n || d.name_source != src)) {
                d.name = n;
                d.name_source = src;
                changed = true;
            }
        }
        return changed;
    };

    /* the sweep: poke every address, read the ARP table every `wave` pokes */
    int poked = 0;
    for (std::uint32_t ho = cfg.range.first; ho <= cfg.range.last && !cancel.cancelled(); ++ho) {
        if (ho != cfg.range.self) prober.poke(ho);
        ++poked;
        snap.probed = poked;
        if (poked % std::max(1, cfg.wave) == 0 || ho == cfg.range.last) {
            prober.service();
            bool changed = merge_arp();
            changed |= merge_rtt();
            changed |= merge_names();
            snap.found = static_cast<int>(snap.devices.size());
            if (names) names->service();
            sort_devices(snap.devices);
            snap.naming = names && names->busy();
            emit(); /* progress moved, publish even when the list did not */
            (void)changed;
            if (!cancel.sleep_ms(80)) break; /* let replies and ARP settle; cancellable */
        }
        if (now_ms() >= deadline) break;
    }
    snap.probed = std::min(poked, snap.total);

    /* collection rounds: late ARP answers and the names */
    for (int r = 0; r < std::max(1, cfg.rounds) && !cancel.cancelled() && now_ms() < deadline; ++r) {
        if (!cancel.sleep_ms(std::max(50, cfg.settle_ms))) break;
        prober.service();
        if (names) names->service();
        bool changed = merge_arp();
        changed |= merge_rtt();
        changed |= merge_names();
        snap.found = static_cast<int>(snap.devices.size());
        snap.naming = names && names->busy();
        sort_devices(snap.devices);
        if (changed || !snap.naming) emit();
        if (!snap.naming && !changed && r > 0) break; /* nothing left to learn */
    }

    /* a last pass to settle any remaining names within the budget */
    while (names && names->busy() && !cancel.cancelled() && now_ms() < deadline) {
        if (!cancel.sleep_ms(100)) break;
        names->service();
        if (merge_names()) {
            sort_devices(snap.devices);
            emit();
        }
    }

    snap.naming = false;
    snap.found = static_cast<int>(snap.devices.size());
    sort_devices(snap.devices);
    snap.state = cancel.cancelled() ? DevicesSnapshot::State::Cancelled : DevicesSnapshot::State::Done;
    emit();
    return snap;
}

/* ---------------------------------------------------------------- the session */

DevicesSession::DevicesSession() : arp_(make_proc_arp_reader()), real_(true) {}

DevicesSession::DevicesSession(std::shared_ptr<Prober> prober, std::shared_ptr<NeighbourReader> arp,
                               std::shared_ptr<NameLookup> names)
    : prober_(std::move(prober)), arp_(std::move(arp)), names_(std::move(names)), real_(false)
{
}

bool DevicesSession::start(const NetState &net, const std::string &iface, const std::string &resolver, bool rescan)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    DevicesConfig cfg;
    cfg.range = scan_range_of(net, iface);
    const Iface *i = net.find(iface);
    cfg.self_mac = i ? i->mac : std::string();
    Route r = net.route_of(iface);
    cfg.gateway = r.ok ? r.gateway : (net.route.ok ? net.route.gateway : std::string());
    if (rescan) {
        if (auto prev = channel_.latest()) cfg.keep = prev->devices;
    }

    DevicesSnapshot first;
    first.interface = iface;
    first.subnet = subnet_text(cfg.range);
    first.gateway = cfg.gateway;
    first.range = cfg.range;
    if (!cfg.range.ok) {
        first.state = DevicesSnapshot::State::Failed;
        first.error = cfg.range.problem.empty() ? "No network" : cfg.range.problem;
        channel_.begin(gen, first);
        return false;
    }
    first.state = DevicesSnapshot::State::Scanning;
    first.total = cfg.range.count();
    channel_.begin(gen, first);

    /* the real prober and name lookup open sockets bound to the chosen interface and keep per-run state: each run makes
     * its own on its worker thread (a run that is still ending never shares them with the next one) */
    std::shared_ptr<Prober> injected_prober = prober_;
    std::shared_ptr<NeighbourReader> arp = arp_;
    std::shared_ptr<NameLookup> injected_names = names_;
    const bool real = real_;
    const std::uint64_t started = worker_.start([this, gen, cfg, injected_prober, arp, injected_names, real,
                                                 resolver](const CancelToken &cancel) {
        std::shared_ptr<Prober> prober = real ? std::shared_ptr<Prober>(make_icmp_tcp_prober()) : injected_prober;
        std::shared_ptr<NameLookup> names = real ? std::shared_ptr<NameLookup>(make_name_lookup(resolver)) : injected_names;
        run_devices(cfg, *prober, *arp, names.get(), cancel,
                    [this, gen](const DevicesSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = DevicesSnapshot::State::Failed;
        first.error = "Could not start the scan (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void DevicesSession::stop() { worker_.cancel(); }

} // namespace nt
