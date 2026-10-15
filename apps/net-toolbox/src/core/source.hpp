/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: where the interface reader reads from. The system (/proc, /sys, /etc and getifaddrs), or a fixture
 * folder for the tests and the headless scenes (NET_TOOLBOX_FIXTURE).
 *
 * A fixture folder mirrors the system: <dir>/proc/net/route, <dir>/etc/resolv.conf, <dir>/sys/class/net/wlan0/flags ...
 * and adds
 *   ifaddrs                  what getifaddrs would give: "<interface> inet|inet6 <address>/<prefix>" per line
 *   cmd/<argv joined by _>   a command's recorded output (cmd/iw_dev_wlan0_link), read by the tests
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace nt {

struct IfAddr {
    std::string iface;
    int family = 0;       /* AF_INET, AF_INET6 */
    std::string address;  /* "192.168.68.121", "fe80::1" (no zone) */
    int prefix = 0;
};

class Source {
public:
    virtual ~Source() = default;
    /* the whole file (capped at 256 kB), "" and ok = false when unreadable */
    virtual std::string read(const std::string &path, bool *ok = nullptr) const = 0;
    virtual bool exists(const std::string &path) const = 0;
    virtual std::vector<std::string> list(const std::string &dir) const = 0; /* names, sorted, no . and .. */
    virtual std::vector<IfAddr> addresses() const = 0;
    virtual std::string describe() const = 0;
};

std::unique_ptr<Source> make_system_source();
std::unique_ptr<Source> make_fixture_source(const std::string &dir);

/* NET_TOOLBOX_FIXTURE=<folder>: an absolute folder, or a name under the app's tests/fixtures (host builds). The
 * source, or nullptr when the variable is unset (the system). */
std::unique_ptr<Source> source_from_env(std::string *described = nullptr);

/* "ifaddrs" fixture lines (pure; tests/test_parsers.cpp) */
std::vector<IfAddr> parse_ifaddrs_fixture(const std::string &text);

} // namespace nt
