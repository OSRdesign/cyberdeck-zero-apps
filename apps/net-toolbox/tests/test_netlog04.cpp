/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the saved text of the Local devices scan and the port scan (same style as the other tools).
 */
#include "check.hpp"

#include "devices.hpp"
#include "netlog.hpp"

using namespace nt;

namespace {

bool has(const std::string &text, const std::string &part)
{
    const bool ok = text.find(part) != std::string::npos;
    if (!ok) std::fprintf(stderr, "missing [%s] in:\n%s\n", part.c_str(), text.c_str());
    return ok;
}

} // namespace

void test_netlog_04()
{
    // the devices scan
    {
        DevicesSnapshot s;
        s.state = DevicesSnapshot::State::Done;
        s.interface = "wlan0";
        s.subnet = "192.168.68.0/24";
        s.gateway = "192.168.68.1";
        s.total = 254;
        s.probed = 254;
        Device self;
        self.ip = "192.168.68.121";
        self.ip_order = 0x0;
        self.mac = "b8:27:eb:aa:bb:cc";
        self.vendor = "Raspberry Pi";
        self.is_self = true;
        Device gw;
        gw.ip = "192.168.68.1";
        gw.mac = "aa:bb:cc:00:00:01";
        gw.vendor = "AVM";
        gw.is_gateway = true;
        gw.has_rtt = true;
        gw.rtt_ms = 3.2;
        Device phone;
        phone.ip = "192.168.68.50";
        phone.mac = "a6:11:22:33:44:55";
        phone.vendor_private = true;
        phone.name = "pixel";
        phone.name_source = NameSource::Mdns;
        s.devices = {self, gw, phone};
        s.found = 3;
        const std::string t = format_devices(s);
        CHECK(has(t, "Interface: wlan0\n"));
        CHECK(has(t, "Subnet: 192.168.68.0/24\n"));
        CHECK(has(t, "Gateway: 192.168.68.1\n"));
        CHECK(has(t, "Probed 254 of 254 addresses; 3 devices found\n"));
        CHECK(has(t, "192.168.68.121 (this device)\n"));
        CHECK(has(t, "192.168.68.1 (gateway)\n"));
        CHECK(has(t, "MAC aa:bb:cc:00:00:01\n"));
        CHECK(has(t, "vendor AVM\n"));
        CHECK(has(t, "round trip 3.2 ms\n"));
        CHECK(has(t, "private (random) address\n"));
        CHECK(has(t, "name pixel (mDNS)\n"));
    }
    // a failed scan
    {
        DevicesSnapshot s;
        s.state = DevicesSnapshot::State::Failed;
        s.interface = "wlan0";
        s.error = "No network";
        const std::string t = format_devices(s);
        CHECK(has(t, "Error: No network\n"));
    }
    // the port scan
    {
        PortScanSnapshot s;
        s.state = PortScanSnapshot::State::Done;
        s.address = "192.168.68.50";
        s.full_range = false;
        s.total = 72;
        s.scanned = 72;
        s.open = {{22, "ssh", 4}, {443, "https", 7}};
        s.open_count = 2;
        const std::string t = format_portscan(s);
        CHECK(has(t, "Device: 192.168.68.50\n"));
        CHECK(has(t, "Ports: common\n"));
        CHECK(has(t, "Scanned 72 of 72; 2 open\n"));
        CHECK(has(t, "22 (ssh): open, 4 ms\n"));
        CHECK(has(t, "443 (https): open, 7 ms\n"));
    }
}
