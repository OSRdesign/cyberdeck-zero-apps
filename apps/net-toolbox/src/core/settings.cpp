/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the settings file (see settings.hpp).
 */
#include "settings.hpp"

#include "dns.hpp"
#include "httpcheck.hpp"
#include "netaddr.hpp"
#include "portcheck.hpp"
#include "sock.hpp"
#include "text.hpp"
#include "whois.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace nt {
namespace {

bool make_dirs(const std::string &path)
{
    if (path.empty()) return false;
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

bool host_ok(const std::string &s)
{
    return !s.empty() && s.size() <= 253 && valid_host(s);
}

bool int_in(const std::string &value, int lo, int hi, int &out)
{
    int v = 0;
    if (!parse_int(value, v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

/* an interface name as the kernel allows it (at most 15 bytes, no blank, no '/') */
bool iface_ok(const std::string &s)
{
    if (s.empty() || s.size() > 15) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_' || c == '@' || c == ':'))
            return false;
    return true;
}

bool speed_ping_ok(const std::string &s)
{
    return s == "gateway" || s == "off" || host_ok(s);
}

} // namespace

bool Settings::save_machine(const WolMachine &input, std::string *why)
{
    WolMachine m = input;
    if (!normalize_machine(m, why)) return false;
    for (WolMachine &old : wol_machines)
        if (lower(old.name) == lower(m.name)) {
            old = m;
            return true;
        }
    if (wol_machines.size() >= kMaxWolMachines) {
        if (why) *why = "At most 20 machines can be saved: remove one first.";
        return false;
    }
    wol_machines.push_back(m);
    return true;
}

bool Settings::remove_machine(const std::string &name)
{
    const std::string key = lower(trim(name));
    const std::size_t before = wol_machines.size();
    wol_machines.erase(std::remove_if(wol_machines.begin(), wol_machines.end(),
                                      [&](const WolMachine &m) { return lower(m.name) == key; }),
                       wol_machines.end());
    return wol_machines.size() != before;
}

const WolMachine *Settings::find_machine(const std::string &name) const
{
    const std::string key = lower(trim(name));
    for (const WolMachine &m : wol_machines)
        if (lower(m.name) == key) return &m;
    return nullptr;
}

bool Settings::add_target(const std::string &input)
{
    const std::string host = trim(input);
    if (!host_ok(host)) return false;
    targets.erase(std::remove(targets.begin(), targets.end(), host), targets.end());
    targets.push_back(host);
    while (targets.size() > kMaxTargets) targets.erase(targets.begin());
    return true;
}

bool Settings::remove_target(const std::string &input)
{
    const std::string host = trim(input);
    const std::size_t before = targets.size();
    targets.erase(std::remove(targets.begin(), targets.end(), host), targets.end());
    return targets.size() != before;
}

std::string Settings::serialize() const
{
    std::string out = "v=1\n";
    for (const std::string &t : targets) out += "target=" + t + "\n";
    if (!ping_target.empty()) out += "ping_target=" + ping_target + "\n";
    out += "ping_interval=" + std::to_string(ping_interval_ms) + "\n";
    out += "ping_timeout=" + std::to_string(ping_timeout_ms) + "\n";
    out += "ping_count=" + std::to_string(ping_count) + "\n";
    if (!dns_name.empty()) out += "dns_name=" + dns_name + "\n";
    out += "dns_type=" + dns_type + "\n";
    if (!dns_resolver.empty()) out += "dns_resolver=" + dns_resolver + "\n";
    if (!port_host.empty()) out += "port_host=" + port_host + "\n";
    out += "port=" + std::to_string(port) + "\n";
    if (!port_preset.empty()) out += "port_preset=" + port_preset + "\n";
    if (!trace_target.empty()) out += "trace_target=" + trace_target + "\n";
    if (!http_url.empty()) out += "http_url=" + http_url + "\n";
    out += std::string("http_insecure=") + (http_insecure ? "1" : "0") + "\n";
    if (!whois_query.empty()) out += "whois_query=" + whois_query + "\n";
    for (const WolMachine &m : wol_machines) out += "wol=" + machine_to_setting(m) + "\n";
    if (!wol_iface.empty()) out += "wol_iface=" + wol_iface + "\n";
    out += std::string("wol_watch=") + (wol_watch ? "1" : "0") + "\n";
    out += "speed_ping=" + speed_ping + "\n";
    out += "ble_seconds=" + std::to_string(ble_seconds) + "\n";
    if (!interface.empty()) out += "interface=" + interface + "\n";
    return out;
}

Settings Settings::parse(const std::string &text)
{
    Settings s;
    for (const std::string &raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "target") {
            if (host_ok(value) && std::find(s.targets.begin(), s.targets.end(), value) == s.targets.end() &&
                s.targets.size() < kMaxTargets)
                s.targets.push_back(value);
        } else if (key == "ping_target") {
            if (host_ok(value)) s.ping_target = value;
        } else if (key == "ping_interval") {
            int_in(value, 200, 10000, s.ping_interval_ms);
        } else if (key == "ping_timeout") {
            int_in(value, 100, 10000, s.ping_timeout_ms);
        } else if (key == "ping_count") {
            int_in(value, 0, 10000, s.ping_count);
        } else if (key == "dns_name") {
            if (host_ok(value)) s.dns_name = value;
        } else if (key == "dns_type") {
            std::uint16_t t = 0;
            if (dns::type_from_name(value, t) &&
                std::find(dns::lookup_types().begin(), dns::lookup_types().end(), t) != dns::lookup_types().end())
                s.dns_type = dns::type_name(t);
        } else if (key == "dns_resolver") {
            if (value.empty() || is_ip(value)) s.dns_resolver = value;
        } else if (key == "port_host") {
            if (host_ok(value)) s.port_host = value;
        } else if (key == "port") {
            int_in(value, 1, 65535, s.port);
        } else if (key == "port_preset") {
            for (const PortPreset &p : port_presets())
                if (value == p.label) s.port_preset = value;
        } else if (key == "trace_target") {
            if (host_ok(value)) s.trace_target = value;
        } else if (key == "http_url") {
            HttpTarget t;
            if (parse_http_target(value, t)) s.http_url = value;
        } else if (key == "http_insecure") {
            if (value == "0" || value == "1") s.http_insecure = value == "1";
        } else if (key == "whois_query") {
            std::string object;
            bool is_ip = false;
            if (parse_whois_query(value, object, is_ip)) s.whois_query = value;
        } else if (key == "wol") {
            WolMachine m;
            if (machine_from_setting(value, m) && !s.find_machine(m.name) && s.wol_machines.size() < kMaxWolMachines)
                s.wol_machines.push_back(m);
        } else if (key == "wol_iface") {
            if (value.empty() || iface_ok(value)) s.wol_iface = value;
        } else if (key == "wol_watch") {
            if (value == "0" || value == "1") s.wol_watch = value == "1";
        } else if (key == "speed_ping") {
            if (speed_ping_ok(value)) s.speed_ping = value;
        } else if (key == "ble_seconds") {
            int_in(value, kBleSecondsMin, kBleSecondsMax, s.ble_seconds);
        } else if (key == "interface") {
            const std::string v = lower(value);
            if (value.empty() || v == "auto" || v == "automatic") s.interface.clear();
            else if (valid_iface_name(value)) s.interface = value;
        }
    }
    return s;
}

std::string settings_path()
{
    const char *over = std::getenv("NET_TOOLBOX_CONFIG");
    if (over && over[0]) return over;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.config";
    else return std::string();
    return base + "/cyberdeckzero/net-toolbox.conf";
}

Settings load_settings(const std::string &path)
{
    if (path.empty()) return Settings();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return Settings();
    std::string text;
    char buf[512];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 65536) text.append(buf, n);
    std::fclose(f);
    return Settings::parse(text);
}

bool save_settings(const std::string &path, const Settings &settings)
{
    if (path.empty()) return false;
    const std::string dir = parent_of(path);
    if (!dir.empty()) make_dirs(dir);
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const std::string text = settings.serialize();
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const bool synced = fsync(fd) == 0;
    close(fd);
    if (!ok || !synced || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace nt
