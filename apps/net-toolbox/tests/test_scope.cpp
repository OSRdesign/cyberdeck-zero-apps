/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests, 0.3 interface chooser: the scope resolved on the recorded boards (a deck with one Wi-Fi, a Pi 3A+ with
 * a second Wi-Fi adapter on a phone hotspot, wired + Wi-Fi, a cable that is out, an adapter that vanished), the gateway of
 * each interface, nmcli's DNS answer, curl's --interface, SO_BINDTODEVICE on loopback (skipped where the kernel does not
 * allow it), the setting.
 */
#include "check.hpp"

#include "cmdrun.hpp"
#include "dns_client.hpp"
#include "fakes.hpp"
#include "httpcheck.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"
#include "publicip.hpp"
#include "scope.hpp"
#include "settings.hpp"
#include "sock.hpp"
#include "speedtest.hpp"
#include "traceroute.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>

using namespace nt;

namespace {

NetState board(const char *name)
{
    const auto src = make_fixture_source(fixture_dir(name));
    return read_net_state(*src);
}

std::string nmcli(const char *board_name, const char *iface)
{
    return fixture(board_name, (std::string("cmd/nmcli_dns_") + iface).c_str());
}

using Strings = std::vector<std::string>;

} // namespace

void test_scope()
{
    // ---- the names
    CHECK(valid_iface_name("wlan0") && valid_iface_name("end0") && valid_iface_name("enx00e04c680a12"));
    CHECK(valid_iface_name("eth0.100") && valid_iface_name("eth0:1") && valid_iface_name("br-lan") && valid_iface_name("a_b"));
    CHECK(!valid_iface_name("") && !valid_iface_name("0123456789abcdef")); /* 16 bytes: one too many */
    CHECK(valid_iface_name("0123456789abcde"));
    CHECK(!valid_iface_name("wlan0;rm") && !valid_iface_name("wlan 0") && !valid_iface_name("-x") && valid_iface_name("x-"));
    CHECK(!valid_iface_name("wlan0\n") && !valid_iface_name("a/b") && !valid_iface_name("$(x)") && !valid_iface_name("wl@n"));
    CHECK(!valid_iface_name("172.20.10.2") && !valid_iface_name("10.0") && !valid_iface_name("1234"));

    // ---- scope resolution
    {
        const NetState st = board("deck"); /* one Wi-Fi */
        NetScope s = resolve_scope("", st);
        CHECK(s.automatic() && !s.missing && s.iface.empty() && s.main == "wlan0" && s.effective() == "wlan0");
        CHECK(s.note().empty() && s.via().empty());
        s = resolve_scope("wlan0", st);
        CHECK(!s.automatic() && !s.missing && s.iface == "wlan0" && s.effective() == "wlan0" && s.via() == "via wlan0");
        // an adapter that is gone: Automatic, with the words, and the choice is kept
        s = resolve_scope("wlan1", st);
        CHECK(s.automatic() && s.missing && s.chosen == "wlan1" && s.iface.empty());
        CHECK_EQ(s.note(), std::string("wlan1 not found: using Automatic"));
        CHECK(s.effective() == "wlan0" && s.via().empty());
        // never a crash on nonsense: the loopback, an invalid name, a name that is not there
        for (const char *bad : {"lo", "no such", "wlan0;x", "../x", "0123456789abcdef"}) {
            s = resolve_scope(bad, st);
            CHECK(s.automatic() && s.missing && s.effective() == "wlan0");
        }
        // no network at all
        const NetState off = board("deck-offline");
        s = resolve_scope("wlan0", off);
        CHECK(!s.automatic() && s.iface == "wlan0" && s.main.empty()); /* chosen and there, just not connected */
        s = resolve_scope("", off);
        CHECK(s.automatic() && s.effective().empty());
    }
    {
        const NetState st = board("pi-3a-plus-dongle"); /* wlan0 (default route) and wlan1, on different networks */
        CHECK(st.main_iface == "wlan0" && st.default_routes.size() == 2);
        NetScope s = resolve_scope("wlan1", st);
        CHECK(s.iface == "wlan1" && s.main == "wlan0" && s.effective() == "wlan1" && !s.missing);
        CHECK_EQ(scope_gateway(s, st), std::string("172.20.10.1"));
        CHECK_EQ(scope_gateway(resolve_scope("wlan0", st), st), std::string("192.168.68.1"));
        CHECK_EQ(scope_gateway(resolve_scope("", st), st), std::string("192.168.68.1")); /* Automatic: the system's */
        CHECK(st.route_of("wlan1").ok && st.route_of("wlan1").metric == 601 && st.route_of("wlan0").metric == 600);
        CHECK(!st.route_of("eth0").ok && !st.route_of("").ok);
        // the adapter unplugged and plugged in again: the same setting, found again
        const NetState without = board("pi-3a-plus");
        s = resolve_scope("wlan1", without);
        CHECK(s.missing && s.automatic() && s.note() == "wlan1 not found: using Automatic");
        s = resolve_scope("wlan1", st);
        CHECK(!s.missing && s.iface == "wlan1");
    }
    {
        const NetState st = board("pi-3a-plus-dongle-off"); /* the dongle is there, joined to nothing */
        const NetScope s = resolve_scope("wlan1", st);
        CHECK(s.iface == "wlan1" && !st.find("wlan1")->connected());
        CHECK_EQ(scope_gateway(s, st), std::string()); /* no route through it */
    }
    {
        const NetState st = board("eth-wifi"); /* wired (metric 100) and Wi-Fi (600) */
        CHECK(st.main_iface == "end0");
        CHECK(st.find("end0")->connected() && !st.find("end0")->wireless && st.find("end0")->speed_mbps == 1000);
        CHECK_EQ(scope_gateway(resolve_scope("end0", st), st), std::string("192.168.1.1"));
        CHECK_EQ(scope_gateway(resolve_scope("wlan0", st), st), std::string("192.168.1.1"));
        CHECK(resolve_scope("wlan0", st).effective() == "wlan0" && resolve_scope("", st).effective() == "end0");
    }
    {
        const NetState st = board("eth-down"); /* the cable of the USB adapter is out; Wi-Fi is up and the default route */
        const Iface *usb = st.find("enx00e04c680a12");
        CHECK(usb && !usb->connected() && !usb->wireless && usb->up && usb->ipv4.empty() && usb->speed_mbps < 0);
        CHECK(st.main_iface == "wlan0");
        const NetScope s = resolve_scope("enx00e04c680a12", st);
        CHECK(s.iface == "enx00e04c680a12" && !s.missing);
        CHECK_EQ(scope_gateway(s, st), std::string());
    }

    // ---- routes: every default route, the best first
    {
        const std::vector<Route> all = parse_default_routes(fixture("pi-3a-plus-dongle", "proc/net/route"));
        CHECK(all.size() == 2 && all[0].iface == "wlan0" && all[0].gateway == "192.168.68.1" && all[0].metric == 600);
        CHECK(all[1].iface == "wlan1" && all[1].gateway == "172.20.10.1" && all[1].metric == 601);
        CHECK(parse_default_route(fixture("pi-3a-plus-dongle", "proc/net/route")).iface == "wlan0");
        // a lower metric on the second line wins, equal metrics keep the file's order
        const std::string two = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
                                "wlan1\t00000000\t010A14AC\t0003\t0\t0\t50\t00000000\t0\t0\t0\n"
                                "wlan0\t00000000\t0144A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                                "eth0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                                "eth1\t0001A8C0\t00000000\t0001\t0\t0\t10\t00FFFFFF\t0\t0\t0\n"; /* not a default route */
        const std::vector<Route> r = parse_default_routes(two);
        CHECK(r.size() == 3 && r[0].iface == "wlan1" && r[1].iface == "wlan0" && r[2].iface == "eth0");
        CHECK(parse_default_routes("").empty() && parse_default_routes("garbage\n1 2 3\n").empty());
    }

    // ---- nmcli's DNS answer
    CHECK((nmcli_dns_argv("wlan1") == Strings{"nmcli", "-g", "IP4.DNS,IP6.DNS", "device", "show", "wlan1"}));
    {
        // as read on the Pi 3A+ for wlan0: the IPv4 line, then the IPv6 line with escaped colons and a link-local one
        const std::string real = "192.168.1.1 | 192.168.68.1\n"
                                 "2a01\\:cb00\\:212\\:1700\\:aecf\\:7bff\\:fe19\\:bff0 | fe80\\:\\:aecf\\:7bff\\:fe19\\:bff0\n";
        CHECK((parse_nmcli_dns(real) == Strings{"192.168.1.1", "192.168.68.1", "2a01:cb00:212:1700:aecf:7bff:fe19:bff0"}));
        CHECK((parse_nmcli_dns(nmcli("pi-3a-plus-dongle", "wlan0")) == parse_nmcli_dns(real)));
        CHECK((parse_nmcli_dns("172.20.10.1\n\n") == Strings{"172.20.10.1"}));
        CHECK(parse_nmcli_dns("").empty() && parse_nmcli_dns("\n\n").empty() && parse_nmcli_dns("  \r\n").empty());
        CHECK((parse_nmcli_dns("1.1.1.1\r\n8.8.8.8\r\n") == Strings{"1.1.1.1", "8.8.8.8"}));
        CHECK((parse_nmcli_dns("1.1.1.1 | 1.1.1.1\n1.1.1.1\n") == Strings{"1.1.1.1"})); /* no duplicate */
        // `nmcli -t` form, and noise that is not an address
        CHECK((parse_nmcli_dns("IP4.DNS[1]:9.9.9.9\nIP4.DNS[2]:149.112.112.112\nIP6.DNS[1]:2620\\:fe\\:\\:fe\n") ==
               Strings{"9.9.9.9", "149.112.112.112", "2620:fe::fe"}));
        CHECK(parse_nmcli_dns("Error: Device 'wlan1' not found.\n--\n| |\n999.1.1.1\nexample.com\n").empty());
        CHECK((parse_nmcli_dns("| 1.1.1.1 ||  | 8.8.8.8 |") == Strings{"1.1.1.1", "8.8.8.8"}));
        std::string many;
        for (int i = 1; i <= 20; ++i) many += "10.0.0." + std::to_string(i) + (i % 2 ? " | " : "\n");
        CHECK(parse_nmcli_dns(many).size() == 8);
    }

    // ---- which DNS servers a scope uses
    {
        const NetState st = board("pi-3a-plus-dongle");
        const NetScope automatic = resolve_scope("", st), w1 = resolve_scope("wlan1", st);
        ScopeDns d = scope_dns(automatic, st, "", false);
        CHECK(d.from == ScopeDns::From::System && d.servers == st.dns.servers && d.label == "System default");
        d = scope_dns(automatic, st, "9.9.9.9\n", true); /* Automatic never asks the interface */
        CHECK(d.from == ScopeDns::From::System && d.servers.front() == "192.168.1.1");
        d = scope_dns(w1, st, nmcli("pi-3a-plus-dongle", "wlan1"), true);
        CHECK(d.from == ScopeDns::From::Interface && d.servers == Strings{"172.20.10.1"} && d.label == "wlan1's DNS");
        // nmcli could not run, or answered nothing: the interface's own gateway
        d = scope_dns(w1, st, "", false);
        CHECK(d.from == ScopeDns::From::Gateway && d.servers == Strings{"172.20.10.1"} && d.label == "wlan1's gateway");
        d = scope_dns(w1, st, "\n\n", true);
        CHECK(d.from == ScopeDns::From::Gateway && d.servers == Strings{"172.20.10.1"});
        // no gateway either: the system's, said to be not the interface's
        const NetState off = board("pi-3a-plus-dongle-off");
        d = scope_dns(resolve_scope("wlan1", off), off, "", true);
        CHECK(d.from == ScopeDns::From::SystemFallback && d.servers == off.dns.servers);
        CHECK_EQ(d.label, std::string("System default (not wlan1's)"));
    }
    {
        const NetState st = board("eth-wifi"); /* a wired interface chosen: its DNS, its gateway */
        const NetScope end0 = resolve_scope("end0", st);
        ScopeDns d = scope_dns(end0, st, nmcli("eth-wifi", "end0"), true);
        CHECK(d.from == ScopeDns::From::Interface && d.servers == Strings{"192.168.1.1"} && d.label == "end0's DNS");
        d = scope_dns(end0, st, "", false);
        CHECK(d.from == ScopeDns::From::Gateway && d.servers == Strings{"192.168.1.1"});
        const NetState down = board("eth-down"); /* cable out: nothing of its own */
        d = scope_dns(resolve_scope("enx00e04c680a12", down), down, nmcli("eth-down", "enx00e04c680a12"), true);
        CHECK(d.from == ScopeDns::From::SystemFallback && d.servers == down.dns.servers);
    }

    // ---- curl --interface: added at the one place, validated, the argv stays an array
    {
        HttpTarget t;
        CHECK(parse_http_target("https://example.com/a?b=c", t));
        HttpCheckRequest req;
        req.url = t.url;
        const Strings plain = curl_check_argv(t, req);
        const Strings bound = with_curl_interface(plain, "wlan1");
        CHECK(bound.size() == plain.size() + 2 && bound[0] == "curl" && bound[1] == "--interface" && bound[2] == "wlan1");
        CHECK(std::equal(plain.begin() + 1, plain.end(), bound.begin() + 3)); /* nothing else moved */
        CHECK(bound.back() == t.url && bound[bound.size() - 2] == "--url");
        CHECK(with_curl_interface(plain, "") == plain); /* Automatic: unchanged */
        for (const char *bad : {"wlan0;reboot", "wlan 0", "$(id)", "a\nb", "wlan0 --proxy x", "0123456789abcdef", "172.20.10.2", "-x"}) {
            CHECK(with_curl_interface(plain, bad) == plain);
        }
        CHECK(with_curl_interface(plain, "wlan0;reboot") == plain);
        CHECK(with_curl_interface(with_curl_interface(plain, "wlan1"), "wlan2") == bound); /* never twice */
        CHECK((with_curl_interface({"openssl", "s_client"}, "wlan1") == Strings{"openssl", "s_client"})); /* only curl */
        CHECK((with_curl_interface({"/usr/bin/curl", "-s"}, "eth0") == Strings{"/usr/bin/curl", "--interface", "eth0", "-s"}));
        CHECK(with_curl_interface({}, "wlan1").empty());
        // the other tools' command lines
        const Strings ip4 = with_curl_interface(publicip_argv(4), "wlan1");
        CHECK(ip4[1] == "--interface" && ip4[2] == "wlan1" && ip4[3] == "-4");
        const Strings down = with_curl_interface(speed_download_argv(1000000, 100000), "end0");
        CHECK(down[1] == "--interface" && down[2] == "end0" && speed_download_total(down) == speed_download_total(speed_download_argv(1000000, 100000)));
        CHECK(with_curl_interface(speed_latency_argv(5), "wlan1")[2] == "wlan1");
        CHECK(with_curl_interface(speed_upload_argv(), "wlan1")[2] == "wlan1");
        // the chosen network of the process
        CHECK(scoped_argv(plain) == plain);
        {
            ScopedActiveInterface use("wlan1");
            CHECK(scoped_argv(plain) == bound);
        }
        CHECK(scoped_argv(plain) == plain && active_interface().empty());
    }
    {
        // the one place that starts a command: a fake curl prints the arguments it was started with
        const std::string dir = test_tmpdir("scope");
        const std::string script = dir + "/curl";
        {
            std::ofstream out(script);
            out << "#!/bin/sh\necho \"args: $*\"\n";
        }
        chmod(script.c_str(), 0755);
        const char *old = std::getenv("PATH");
        const std::string saved = old ? old : "";
        setenv("PATH", (dir + ":" + saved).c_str(), 1);
        DirectRunner run;
        CancelToken none;
        CommandSpec spec;
        spec.argv = {"curl", "-sS", "https://example.com"};
        spec.timeout_ms = 5000;
        CommandResult r = run.run(spec, none);
        CHECK(r.ok() && r.output == "args: -sS https://example.com\n");
        {
            ScopedActiveInterface use("wlan1");
            r = run.run(spec, none);
            CHECK(r.ok() && r.output == "args: --interface wlan1 -sS https://example.com\n");
            spec.argv = {"echo", "hello"}; /* another command is left alone */
            r = run.run(spec, none);
            CHECK(r.ok() && r.output == "hello\n");
        }
        setenv("PATH", saved.c_str(), 1);
        remove_dir(dir);
    }

    // ---- SO_BINDTODEVICE
    {
        CHECK(bind_to_device(-1, "") == 0); /* nothing to bind: not even a socket is needed */
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(fd >= 0);
        CHECK(bind_to_device(fd, "wlan0;x") == EINVAL);
        const int lo = bind_to_device(fd, "lo");
        if (lo == EPERM || lo == EACCES) {
            SKIP("SO_BINDTODEVICE is not allowed without a capability on this kernel (older than 5.7)");
        } else {
            CHECK_EQ(lo, 0);
            char name[32] = {};
            socklen_t len = sizeof(name);
            CHECK(getsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, &len) == 0 && std::string(name) == "lo");
            CHECK_EQ(bind_to_device(fd, "nosuchdev0"), ENODEV); /* an adapter that is not there */
            CHECK_EQ(errno_words(ENODEV), std::string("network interface not found"));
        }
        close(fd);
    }
    {
        // the chosen network reaches the sockets of the tools: on loopback it still works, on a name that is not there it
        // says so (and sends nothing)
        fake::Tcp server("hello\r\n\r\n");
        fake::Dns dns;
        Address lo;
        parse_ip("127.0.0.1", lo);
        CancelToken none;
        const int probe = socket(AF_INET, SOCK_DGRAM, 0);
        const int allowed = bind_to_device(probe, "lo");
        close(probe);
        if (!server.ok() || !dns.ok()) {
            SKIP("cannot listen on 127.0.0.1");
        } else if (allowed == EPERM || allowed == EACCES) {
            SKIP("SO_BINDTODEVICE is not allowed on this kernel: the bound sockets are not tested");
        } else {
            dns.add("example.com", dns::kA, {"example.com", dns::kA, 300, fake::rd_a("93.184.215.14")});
            {
                ScopedActiveInterface use("lo");
                CHECK(check_port(lo, server.port(), 1000, none).state == PortState::Open); /* TCP */
                DnsQuery q;
                q.name = "example.com";
                q.server = "127.0.0.1";
                q.port = dns.port();
                q.timeout_ms = 500;
                CHECK(dns_query(q, none).ok()); /* UDP */
                dns.set_mode(fake::Dns::Mode::Truncate);
                const DnsResult tcp = dns_query(q, none); /* UDP answer truncated: asked again over TCP, bound too */
                CHECK(tcp.ok() && tcp.via_tcp);
                dns.set_mode(fake::Dns::Mode::Answer);
                int err = 0;
                CHECK(open_probe_socket(lo, ProbeMethod::Udp, &err) != nullptr); /* traceroute */
            }
            {
                ScopedActiveInterface use("nosuchdev0");
                const PortResult r = check_port(lo, server.port(), 1000, none);
                CHECK(r.state != PortState::Open && r.err == ENODEV && r.detail == "network interface not found");
                DnsQuery q;
                q.name = "example.com";
                q.server = "127.0.0.1";
                q.port = dns.port();
                q.timeout_ms = 300;
                const DnsResult d = dns_query(q, none);
                CHECK(!d.answered && d.error == "network interface not found");
                int err = 0;
                CHECK(open_probe_socket(lo, ProbeMethod::Udp, &err) == nullptr && err == ENODEV);
                if (ping_available().ipv4) {
                    err = 0;
                    CHECK(open_icmp_echo(lo, 56, &err) == nullptr && err == ENODEV);
                }
            }
            if (ping_available().ipv4) {
                int err = 0;
                ScopedActiveInterface use("lo");
                CHECK(open_icmp_echo(lo, 56, &err) != nullptr && err == 0);
            } else {
                SKIP("ICMP datagram sockets are not allowed on this host: the bound echo socket is not tested");
            }
        }
    }

    // ---- the setting
    {
        CHECK(Settings().interface.empty());
        Settings s = Settings::parse("v=1\ninterface=wlan1\nport=22\n");
        CHECK_EQ(s.interface, std::string("wlan1"));
        CHECK_EQ(Settings::parse(s.serialize()).interface, std::string("wlan1"));
        CHECK(s.serialize().find("interface=wlan1\n") != std::string::npos);
        // Automatic is not written; a 0.2 / 0.3-before file has no key and reads as Automatic
        s.interface.clear();
        CHECK(s.serialize().find("interface=") == std::string::npos);
        CHECK(Settings::parse("v=1\nping_target=8.8.8.8\n").interface.empty());
        CHECK(Settings::parse("interface=\n").interface.empty());
        CHECK(Settings::parse("interface=auto\n").interface.empty() && Settings::parse("interface=Automatic\n").interface.empty());
        // bad names are ignored (the earlier valid value stays), never executed
        CHECK_EQ(Settings::parse("interface=eth0\ninterface=wlan0;reboot\n").interface, std::string("eth0"));
        CHECK(Settings::parse("interface=a b\n").interface.empty() && Settings::parse("interface=172.20.10.2\n").interface.empty());
        CHECK_EQ(Settings::parse("interface=enx00e04c680a12\n").interface, std::string("enx00e04c680a12"));
        // the other keys do not care, a round trip keeps them
        s = Settings::parse("interface=wlan1\nwol_iface=wlan0\nspeed_ping=gateway\n");
        const Settings back = Settings::parse(s.serialize());
        CHECK(back.interface == "wlan1" && back.wol_iface == "wlan0" && back.speed_ping == "gateway");
    }
}
