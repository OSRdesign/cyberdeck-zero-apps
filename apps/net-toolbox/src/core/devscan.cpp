/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the Local devices port scan of one device (see devices.hpp). A non-blocking TCP connect scan with
 * bounded concurrency, bound to the chosen interface, of one private address only.
 */
#include "devices.hpp"

#include "portcheck.hpp"
#include "sock.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace nt {

const std::vector<std::pair<std::uint16_t, const char *>> &local_common_ports()
{
    /* a curated list of the services found on a home / lab LAN, with their usual names (extends port_presets) */
    static const std::vector<std::pair<std::uint16_t, const char *>> ports = {
        {21, "ftp"},          {22, "ssh"},         {23, "telnet"},     {25, "smtp"},         {53, "dns"},
        {80, "http"},         {81, "http-alt"},    {88, "kerberos"},   {110, "pop3"},        {111, "rpcbind"},
        {119, "nntp"},        {123, "ntp"},        {135, "msrpc"},     {139, "netbios-ssn"}, {143, "imap"},
        {161, "snmp"},        {389, "ldap"},       {427, "slp"},       {443, "https"},       {445, "smb"},
        {465, "smtps"},       {515, "printer"},    {548, "afp"},       {554, "rtsp"},        {587, "submission"},
        {631, "ipp"},         {636, "ldaps"},      {873, "rsync"},     {993, "imaps"},       {995, "pop3s"},
        {1080, "socks"},      {1194, "openvpn"},   {1433, "mssql"},    {1723, "pptp"},       {1883, "mqtt"},
        {1900, "upnp"},       {2049, "nfs"},       {2222, "ssh-alt"},  {2375, "docker"},     {3000, "dev-web"},
        {3128, "squid"},      {3306, "mysql"},     {3389, "rdp"},      {3689, "daap"},       {5000, "upnp/web"},
        {5001, "synology"},   {5060, "sip"},       {5353, "mdns"},     {5432, "postgres"},   {5555, "adb"},
        {5900, "vnc"},        {6379, "redis"},     {6443, "k8s-api"},  {7000, "airplay"},    {7547, "tr-069"},
        {8000, "http-alt"},   {8006, "proxmox"},   {8008, "http-alt"}, {8080, "http-proxy"}, {8086, "influxdb"},
        {8096, "jellyfin"},   {8123, "homeassist"},{8200, "dlna"},     {8443, "https-alt"},  {8554, "rtsp-alt"},
        {8728, "mikrotik"},   {8888, "http-alt"},  {9000, "alt"},      {9090, "cockpit"},    {9100, "jetdirect"},
        {9200, "elastic"},    {10000, "webmin"},   {27017, "mongodb"}, {32400, "plex"},      {51820, "wireguard"},
        {62078, "iphone-sync"},
    };
    return ports;
}

const char *local_port_service(std::uint16_t port)
{
    const char *svc = port_service(port);
    if (svc[0]) return svc;
    for (const auto &p : local_common_ports())
        if (p.first == port) return p.second;
    return "";
}

namespace {

/* "" = the target may be scanned (see scan_target_problem); allow_loopback is the tests' override for 127.x listeners */
std::string target_problem(const Address &a, const ScanRange &range, bool allow_loopback)
{
    if (allow_loopback && a.family == AF_INET && is_loopback(a)) return std::string();
    return scan_target_problem(a, range);
}

std::vector<std::uint16_t> ports_to_scan(bool full_range)
{
    std::vector<std::uint16_t> list;
    if (full_range) {
        for (int p = 1; p <= 1024; ++p) list.push_back(static_cast<std::uint16_t>(p));
    } else {
        for (const auto &p : local_common_ports()) list.push_back(p.first);
    }
    return list;
}

struct InFlight {
    Fd fd;
    std::uint16_t port = 0;
    std::int64_t start_us = 0;
    std::int64_t deadline_ms = 0;
};

void add_open(PortScanSnapshot &snap, std::uint16_t port, int connect_ms)
{
    OpenPort o;
    o.port = port;
    o.service = local_port_service(port);
    o.connect_ms = connect_ms;
    snap.open.push_back(o);
    std::sort(snap.open.begin(), snap.open.end(),
              [](const OpenPort &a, const OpenPort &b) { return a.port < b.port; });
    snap.open_count = static_cast<int>(snap.open.size());
}

} // namespace

PortScanSnapshot run_port_scan(const PortScanConfig &input, const CancelToken &cancel,
                               const std::function<void(const PortScanSnapshot &)> &publish)
{
    PortScanConfig cfg = input;
    cfg.timeout_ms = std::min(5000, std::max(100, cfg.timeout_ms));
    cfg.concurrency = std::min(256, std::max(1, cfg.concurrency));

    PortScanSnapshot snap;
    snap.address = cfg.address.text();
    snap.full_range = cfg.full_range;
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    /* only a private / link-local host of the scanned subnet: checked here, before any socket is made */
    const std::string problem = target_problem(cfg.address, cfg.range, cfg.allow_loopback);
    if (!problem.empty()) {
        snap.state = PortScanSnapshot::State::Failed;
        snap.error = problem;
        emit();
        return snap;
    }
    const std::vector<std::uint16_t> ports = cfg.ports.empty() ? ports_to_scan(cfg.full_range) : cfg.ports;
    snap.total = static_cast<int>(ports.size());
    snap.state = PortScanSnapshot::State::Scanning;
    emit();

    std::vector<InFlight> flight;
    std::size_t next = 0;
    std::int64_t last_emit = now_ms();

    const auto start_one = [&](std::uint16_t port) {
        Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
        if (!fd.valid() || bind_to_active_device(fd.get()) != 0) {
            ++snap.scanned; /* could not even start: counts as done, not open */
            return;
        }
        sockaddr_storage ss;
        const socklen_t sl = cfg.address.to_sockaddr(port, ss);
        const std::int64_t t0 = now_us();
        if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&ss), sl) == 0) {
            add_open(snap, port, static_cast<int>((now_us() - t0 + 500) / 1000));
            ++snap.scanned;
            return;
        }
        if (errno != EINPROGRESS) {
            ++snap.scanned; /* refused / unreachable at once: closed */
            return;
        }
        InFlight f;
        f.fd = std::move(fd);
        f.port = port;
        f.start_us = t0;
        f.deadline_ms = now_ms() + cfg.timeout_ms;
        flight.push_back(std::move(f));
    };

    while (!cancel.cancelled() && (next < ports.size() || !flight.empty())) {
        while (static_cast<int>(flight.size()) < cfg.concurrency && next < ports.size() && !cancel.cancelled())
            start_one(ports[next++]);

        std::vector<pollfd> pfds;
        pfds.reserve(flight.size());
        for (const InFlight &f : flight) pfds.push_back({f.fd.get(), POLLOUT, 0});
        const int rc = pfds.empty() ? 0 : poll(pfds.data(), pfds.size(), kSliceMs);

        const std::int64_t now = now_ms();
        for (std::size_t i = 0; i < flight.size();) {
            InFlight &f = flight[i];
            bool resolved = false;
            if (rc > 0 && (pfds[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
                int so = 0;
                socklen_t len = sizeof(so);
                if (getsockopt(f.fd.get(), SOL_SOCKET, SO_ERROR, &so, &len) != 0) so = errno;
                if (so == 0) add_open(snap, f.port, static_cast<int>((now_us() - f.start_us + 500) / 1000));
                resolved = true; /* open or closed: done either way */
            } else if (now >= f.deadline_ms) {
                resolved = true; /* no answer: filtered */
            }
            if (resolved) {
                ++snap.scanned;
                flight.erase(flight.begin() + static_cast<std::ptrdiff_t>(i));
                pfds.erase(pfds.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        if (now_ms() - last_emit >= 100 || (next >= ports.size() && flight.empty())) {
            last_emit = now_ms();
            emit();
        }
    }

    snap.state = cancel.cancelled() ? PortScanSnapshot::State::Cancelled : PortScanSnapshot::State::Done;
    if (!cancel.cancelled()) snap.scanned = snap.total;
    emit();
    return snap;
}

bool PortScanSession::start(const Address &address, bool full_range, const ScanRange &range)
{
    worker_.cancel();
    const std::uint64_t gen = worker_.generation() + 1;
    PortScanSnapshot first;
    first.address = address.text();
    first.full_range = full_range;
    /* the session always needs the scanned subnet (the DevicesSnapshot's range); the override is for the tests only */
    const bool loopback_test = allow_loopback_ && address.family == AF_INET && is_loopback(address);
    const std::string problem = !range.ok && !loopback_test ? std::string("No network")
                                                            : target_problem(address, range, allow_loopback_);
    if (!problem.empty()) {
        first.state = PortScanSnapshot::State::Failed;
        first.error = problem;
        channel_.begin(gen, first);
        return false;
    }
    first.state = PortScanSnapshot::State::Scanning;
    first.total = static_cast<int>(ports_to_scan(full_range).size());
    channel_.begin(gen, first);
    PortScanConfig cfg;
    cfg.address = address;
    cfg.range = range;
    cfg.full_range = full_range;
    cfg.allow_loopback = allow_loopback_;
    const std::uint64_t started = worker_.start([this, gen, cfg](const CancelToken &cancel) {
        run_port_scan(cfg, cancel, [this, gen](const PortScanSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = PortScanSnapshot::State::Failed;
        first.error = "Could not start the scan (no thread)";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void PortScanSession::stop() { worker_.cancel(); }

} // namespace nt
