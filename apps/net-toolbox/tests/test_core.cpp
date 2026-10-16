/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of Net Toolbox's core/ (no LVGL, no screen) and of its package files (.desktop, app.json). Built and run
 * by ../../_common/build_app.sh host (make -C _common/headless): this file has main(), the package, the log files,
 * the settings and the worker; the other .cpp files of tests/ the groups of check.hpp.
 */
#include "check.hpp"

#include "netlog.hpp"
#include "settings.hpp"
#include "text.hpp"
#include "toolbox.hpp"
#include "worker.hpp"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

int g_pass = 0, g_fail = 0, g_skip = 0;

using namespace nt;

static std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string fixture_dir(const char *board) { return std::string(CDK_APP_DIR) + "/tests/fixtures/" + board; }

std::string fixture(const char *board, const char *path) { return read_file(fixture_dir(board) + "/" + path); }

std::string test_tmpdir(const char *tag)
{
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/nt-test-" + tag + "-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string("/tmp");
}

static int remove_entry(const char *path, const struct stat *, int, struct FTW *)
{
    return remove(path);
}

void remove_dir(const std::string &dir)
{
    if (dir.find("/nt-test-") != std::string::npos) /* only what test_tmpdir() made */
        nftw(dir.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

void test_text_helpers()
{
    CHECK_EQ(fmt_ms(0.42), std::string("0.4 ms"));
    CHECK_EQ(fmt_ms(3.25), std::string("3.2 ms"));
    CHECK_EQ(fmt_ms(14.4), std::string("14 ms"));
    CHECK_EQ(fmt_ms(1203.0), std::string("1203 ms"));
    CHECK_EQ(fmt_ms(-1), std::string("0.0 ms"));
    CHECK_EQ(fmt_pct(0), std::string("0%"));
    CHECK_EQ(fmt_pct(2.5), std::string("2.5%"));
    CHECK_EQ(fmt_pct(100.0 / 3), std::string("33%"));
    CHECK_EQ(fmt_speed(100), std::string("100 Mbit/s"));
    CHECK_EQ(fmt_speed(1000), std::string("1 Gbit/s"));
    CHECK_EQ(fmt_speed(2500), std::string("2.5 Gbit/s"));
    CHECK_EQ(fmt_speed(-1), std::string(""));
    CHECK_EQ(fmt_mbit(72.2), std::string("72.2 Mbit/s"));
    CHECK_EQ(fmt_mbit(150), std::string("150 Mbit/s"));
    std::uint32_t h = 0;
    CHECK(parse_hex("0x1003", h) && h == 0x1003);
    CHECK(parse_hex("0144A8C0", h) && h == 0x0144A8C0u);
    CHECK(!parse_hex("0x", h) && !parse_hex("12345678A", h) && !parse_hex("zz", h));
    int i = 0;
    CHECK(parse_int(" -42 ", i) && i == -42);
    CHECK(!parse_int("99999999999", i) && !parse_int("4x", i) && !parse_int("", i));
    double d = 0;
    CHECK(parse_double("-58.", d) && d == -58.0);
    CHECK(!parse_double("nan", d) && !parse_double("1e999", d));
    CHECK_EQ(split_ws(" a\tb  c ").size(), static_cast<std::size_t>(3));
    CHECK_EQ(split_lines("a\r\nb\n").size(), static_cast<std::size_t>(2));
    CHECK_EQ(printable(std::string("a\x01" "b", 3)), std::string("a\\x01b"));
}

/* The package: X-MinSize equal to the app's minimum, Exec in root/, app.json complete (a draft or published: the flag is not checked). */
static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop = read_file(app + "/root/usr/share/APPLaunch/applications/net-toolbox.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nName=Net Toolbox\n") != std::string::npos);
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(kMinW) + "x" + std::to_string(kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/net-toolbox\n") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/net-toolbox.png") != std::string::npos); /* the icon itself: pending */
    CHECK(desktop.find("\nTerminal=false") != std::string::npos);
    const std::string copyright = read_file(app + "/root/usr/share/doc/net-toolbox/copyright");
    CHECK(copyright.find("License: MIT") != std::string::npos);
    const std::string manifest = read_file(app + "/app.json");
    CHECK(manifest.find("\"share_code\": \"net-toolbox\"") != std::string::npos);
    CHECK(manifest.find("\"package\": \"net-toolbox\"") != std::string::npos);
    CHECK(manifest.find("\"version\": \"0.4.1\"") != std::string::npos);
    CHECK(manifest.find("\"title\": \"Net Toolbox\"") != std::string::npos);
    CHECK(manifest.find("\"license\": \"MIT\"") != std::string::npos);
    CHECK(manifest.find("\"categories\": [\"Tools\"]") != std::string::npos);
    CHECK(manifest.find("\"permissions\": {\"network\": true}") != std::string::npos);
    CHECK(manifest.find("\"depends\": \"libfreetype6, libinput10, libxkbcommon0, curl, ca-certificates, network-manager\"") !=
          std::string::npos);
    CHECK(manifest.find("libc6") == std::string::npos);
    CHECK(std::string(kAppId) == "net-toolbox");
    CHECK(std::string(kAppTitle) == "Net Toolbox");
}

void test_netlog()
{
    std::time_t when = 1792072800; /* 2026-10-15 14:00 UTC: the name is in local time */
    struct tm tm;
    localtime_r(&when, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    CHECK_EQ(log_file_name("Ping", when), std::string("ping-") + stamp + ".txt");
    CHECK_EQ(log_file_name("Port check", when), std::string("port-check-") + stamp + ".txt");
    CHECK_EQ(log_file_name("../../etc/x", when), std::string("etcx-") + stamp + ".txt");
    CHECK_EQ(log_file_name("", when), std::string("result-") + stamp + ".txt");
    CHECK(log_heading("0.1.0", "Ping", when).find("Net Toolbox 0.1.0 - Ping - ") == 0);

    const std::string dir = test_tmpdir("logs") + "/net-logs/sub";
    SaveResult r = save_log(dir, "ping", "line 1\nline 2\n", when);
    CHECK(r.ok);
    CHECK_EQ(r.path, dir + "/ping-" + stamp + ".txt");
    CHECK_EQ(read_file(r.path), std::string("line 1\nline 2\n"));
    SaveResult r2 = save_log(dir, "ping", "again\n", when); /* the same second */
    CHECK(r2.ok && r2.path == dir + "/ping-" + stamp + "-2.txt");
    CHECK(access((r2.path + ".tmp").c_str(), F_OK) != 0);
    SaveResult bad = save_log("", "ping", "x", when);
    CHECK(!bad.ok && !bad.error.empty());
    SaveResult blocked = save_log(r.path + "/inside", "ping", "x", when); /* a file where the folder should be */
    CHECK(!blocked.ok && blocked.error.find("Cannot create") == 0);

    setenv("NET_TOOLBOX_LOG_DIR", "/x/logs", 1);
    CHECK_EQ(default_log_dir(), std::string("/x/logs"));
    unsetenv("NET_TOOLBOX_LOG_DIR");
    const char *home = std::getenv("HOME");
    setenv("HOME", "/home/deck", 1);
    CHECK_EQ(default_log_dir(), std::string("/home/deck/net-logs"));
    if (home) setenv("HOME", home, 1);
    remove_dir(dir.substr(0, dir.size() - std::string("/net-logs/sub").size()));

    // the text of each tool
    NetState st;
    Iface w;
    w.name = "wlan0";
    w.up = w.running = true;
    w.operstate = "up";
    w.ipv4.push_back("192.168.68.121/22");
    w.link_valid = true;
    w.link.connected = true;
    w.link.ssid = "DeckNet";
    st.ifaces.push_back(w);
    st.route.ok = true;
    st.route.gateway = "192.168.68.1";
    st.route.iface = "wlan0";
    st.dns.servers = {"192.168.1.1", "192.168.68.1"};
    const std::string itext = format_interfaces(st);
    CHECK(itext.find("wlan0: connected (up)\n  IPv4 192.168.68.121/22\n") == 0);
    CHECK(itext.find("Wi-Fi DeckNet") != std::string::npos);
    CHECK(itext.find("Gateway: 192.168.68.1 via wlan0\nDNS servers: 192.168.1.1 192.168.68.1\n") != std::string::npos);

    PingSnapshot ps;
    ps.target = "gw";
    ps.address = "192.168.68.1";
    ps.stats.add_sent();
    ps.stats.add_reply(3.0);
    ps.stats.add_sent();
    ps.stats.add_lost();
    ps.recent = {{1, PacketState::Ok, 3.0}, {2, PacketState::Lost, 0}};
    const std::string ptext = format_ping(ps);
    CHECK(ptext.find("Target: gw (192.168.68.1)\n") == 0);
    CHECK(ptext.find("Packets: 2 sent, 1 received, 1 lost (50% loss)\n") != std::string::npos);
    CHECK(ptext.find("  1 ok 3.0 ms\n  2 lost\n") != std::string::npos);

    PortSnapshot pt;
    pt.host = "example.com";
    pt.address = "93.184.215.14";
    PortResult open;
    open.port = 443;
    open.state = PortState::Open;
    open.connect_ms = 21;
    PortResult filtered;
    filtered.port = 25;
    filtered.state = PortState::Filtered;
    filtered.detail = "timed out";
    pt.results = {open, filtered};
    const std::string ttext = format_ports(pt);
    CHECK(ttext.find("  443 (https): open, 21 ms\n  25 (smtp): filtered, timed out\n") != std::string::npos);

    DiagReport dr;
    dr.steps[0].state = StepState::Pass;
    dr.steps[0].detail = "wlan0";
    dr.explanation = "Because.";
    const std::string dtext = format_diagnosis(dr);
    CHECK(dtext.find("Link up: pass (wlan0)\nAddress: pending\n") == 0);
    CHECK(dtext.find("Because.\n") != std::string::npos);
}

void test_settings()
{
    Settings s = Settings::parse("v=1\ntarget=1.1.1.1\ntarget=gw.lan\ntarget=bad host\ntarget=1.1.1.1\n"
                                 "ping_target=8.8.8.8\nping_interval=500\nping_timeout=99999\nping_count=10\n"
                                 "dns_name=example.com\ndns_type=mx\ndns_resolver=9.9.9.9\nport_host=nas.lan\n"
                                 "port=22\nport_preset=Mail\nnoise\n# c\n");
    CHECK_EQ(s.targets.size(), static_cast<std::size_t>(2));
    CHECK_EQ(s.targets[1], std::string("gw.lan"));
    CHECK_EQ(s.ping_target, std::string("8.8.8.8"));
    CHECK_EQ(s.ping_interval_ms, 500);
    CHECK_EQ(s.ping_timeout_ms, 2000); /* out of range: the default */
    CHECK_EQ(s.ping_count, 10);
    CHECK_EQ(s.dns_type, std::string("MX"));
    CHECK_EQ(s.dns_resolver, std::string("9.9.9.9"));
    CHECK_EQ(s.port_host, std::string("nas.lan"));
    CHECK_EQ(s.port, 22);
    CHECK_EQ(s.port_preset, std::string("Mail"));
    CHECK_EQ(Settings::parse("dns_type=SOA\ndns_resolver=not-an-ip\nport=0\nport_preset=Nope\n").dns_type,
             std::string("A"));
    const Settings d = Settings::parse("dns_resolver=not-an-ip\nport=70000\nport_preset=Nope\n");
    CHECK(d.dns_resolver.empty() && d.port == 443 && d.port_preset.empty());
    // the round trip
    const Settings back = Settings::parse(s.serialize());
    CHECK(back.targets == s.targets && back.ping_target == s.ping_target && back.ping_interval_ms == 500 &&
          back.ping_count == 10 && back.dns_name == s.dns_name && back.dns_type == "MX" &&
          back.dns_resolver == "9.9.9.9" && back.port_host == "nas.lan" && back.port == 22 && back.port_preset == "Mail");
    // saved targets: invalid refused, a repeat moves to the end, at most 8
    Settings t;
    CHECK(!t.add_target("two words"));
    CHECK(t.add_target("a.lan") && t.add_target("b.lan") && t.add_target(" a.lan "));
    CHECK(t.targets.size() == 2 && t.targets.back() == "a.lan");
    for (int i = 0; i < 10; ++i) t.add_target("h" + std::to_string(i) + ".lan");
    CHECK_EQ(t.targets.size(), Settings::kMaxTargets);
    CHECK_EQ(t.targets.back(), std::string("h9.lan"));
    CHECK(t.remove_target("h9.lan") && !t.remove_target("h9.lan"));
    // the file
    const std::string dir = test_tmpdir("settings");
    const std::string path = dir + "/a/b/net-toolbox.conf";
    CHECK(save_settings(path, s));
    CHECK_EQ(load_settings(path).port, 22);
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
    CHECK_EQ(load_settings(dir + "/missing.conf").port, 443);
    setenv("NET_TOOLBOX_CONFIG", path.c_str(), 1);
    CHECK_EQ(settings_path(), path);
    unsetenv("NET_TOOLBOX_CONFIG");
    const char *home = std::getenv("HOME");
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/deck", 1);
    CHECK_EQ(settings_path(), std::string("/home/deck/.config/cyberdeckzero/net-toolbox.conf"));
    setenv("XDG_CONFIG_HOME", "/x", 1);
    CHECK_EQ(settings_path(), std::string("/x/cyberdeckzero/net-toolbox.conf"));
    if (home) setenv("HOME", home, 1);
    if (xdg) setenv("XDG_CONFIG_HOME", xdg, 1);
    else unsetenv("XDG_CONFIG_HOME");
    remove_dir(dir);

    // 0.2: the traceroute target, the HTTP address and its certificate flag, the whois query
    const std::string v01 = "v=1\ntarget=1.1.1.1\nping_target=8.8.8.8\ndns_type=MX\nport=22\n"; /* a 0.1 file */
    Settings old = Settings::parse(v01);
    CHECK(old.trace_target.empty() && old.http_url.empty() && !old.http_insecure && old.whois_query.empty());
    CHECK(old.ping_target == "8.8.8.8" && old.dns_type == "MX" && old.port == 22);
    Settings n = Settings::parse(v01 + "trace_target=example.com\nhttp_url=https://example.com/a?b=1&c=2\n"
                                       "http_insecure=1\nwhois_query=2001:db8::1\n");
    CHECK(n.trace_target == "example.com" && n.http_url == "https://example.com/a?b=1&c=2" && n.http_insecure);
    CHECK(n.whois_query == "2001:db8::1");
    const Settings round = Settings::parse(n.serialize());
    CHECK(round.trace_target == n.trace_target && round.http_url == n.http_url && round.http_insecure &&
          round.whois_query == n.whois_query && round.ping_target == "8.8.8.8");
    const Settings bad = Settings::parse("trace_target=bad host\nhttp_url=file:///etc/passwd\nhttp_insecure=yes\n"
                                         "whois_query=-h\n");
    CHECK(bad.trace_target.empty() && bad.http_url.empty() && !bad.http_insecure && bad.whois_query.empty());
    CHECK(Settings().serialize().find("http_insecure=0\n") != std::string::npos); /* off by default */
}

void test_worker()
{
    // a run, its snapshots, its end
    {
        Worker w;
        Channel<int> ch;
        const std::uint64_t gen = w.generation() + 1;
        ch.begin(gen, 0);
        CHECK(w.start([&ch, gen](const CancelToken &) {
            for (int i = 1; i <= 3; ++i) ch.publish(gen, i);
        }) == gen);
        CHECK(w.wait(2000));
        CHECK(!w.running());
        CHECK_EQ(*ch.latest(), 3);
        CHECK_EQ(ch.seq(), static_cast<std::uint64_t>(4));
    }
    // a new start cancels the old run, whose late snapshots are dropped; start() never waits for it
    {
        Worker w;
        Channel<int> ch;
        std::atomic<bool> old_saw_cancel{false};
        const std::uint64_t g1 = w.generation() + 1;
        ch.begin(g1, 1);
        w.start([&](const CancelToken &c) {
            while (!c.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::this_thread::sleep_for(std::chrono::milliseconds(150)); /* ends slowly */
            old_saw_cancel = true;
            ch.publish(g1, 111); /* stale */
        });
        const std::uint64_t g2 = w.generation() + 1;
        ch.begin(g2, 2);
        const std::int64_t t0 = now_ms();
        CHECK(w.start([&](const CancelToken &) { ch.publish(g2, 222); }) == g2);
        CHECK(now_ms() - t0 < 100); /* the old run was not waited for */
        CHECK(w.wait(2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(old_saw_cancel.load());
        CHECK_EQ(*ch.latest(), 222);
    }
    // cancel() and the token's sleep
    {
        Worker w;
        std::atomic<bool> finished{false};
        w.start([&](const CancelToken &c) {
            c.sleep_ms(10000);
            finished = true;
        });
        const std::int64_t t0 = now_ms();
        w.cancel();
        CHECK(w.wait(1000));
        CHECK(now_ms() - t0 < 500);
        CHECK(finished.load());
    }
    // the destructor joins a run that is still going
    {
        std::atomic<bool> finished{false};
        {
            Worker w;
            w.start([&](const CancelToken &c) {
                c.sleep_ms(10000);
                finished = true;
            });
        }
        CHECK(finished.load());
    }
    CancelToken t;
    CHECK(!t.cancelled());
    t.cancel();
    CHECK(t.cancelled() && !t.sleep_ms(100));
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    test_text_helpers();
    test_netaddr();
    test_interfaces();
    test_dns_wire();
    test_dns_fuzz();
    test_dns_client();
    test_ping_stats();
    test_ping_engine();
    test_ping_real();
    test_portcheck();
    test_http();
    test_diagnosis();
    test_netlog();
    test_settings();
    test_worker();
    test_ui_model();
    test_ui_model_02();
    test_cmdrun();
    test_httpcheck();
    test_publicip();
    test_whois();
    test_trace_engine();
    test_trace_rdns();
    test_trace_real();
    test_netlog_02();
    test_stream_runner();
    test_speedtest();
    test_wol();
    test_ble();
    test_netlog_03();
    test_scope();
    test_ui_scope();
    test_ui_model_03();
    test_subnet_math();
    test_arp_parser();
    test_oui();
    test_lannames();
    test_devices_engine();
    test_devices_guards();
    test_devices_rescan();
    test_devices_failed();
    test_devices_session_cancel();
    test_devscan();
    test_netlog_04();
    test_wifiscan();
    test_walktest();
    test_ui_model_04();
    test_package();
    std::printf("test-net-toolbox: %d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
