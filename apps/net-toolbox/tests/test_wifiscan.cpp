/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): Wi-Fi networks. nmcli's terse output recorded on the Pi 3A+ (tests/fixtures/wifi, sanitized)
 * and made-up lines for the rare cases (escaped ':' and '\', hidden, 5 and 6 GHz, WPA3, Enterprise, Enhanced Open, WEP,
 * ad-hoc, the same SSID on two BSSIDs, a BSSID printed twice, bytes that are not UTF-8, lines that are not networks);
 * NetworkManager's percentage to dBm (against iw's reading); channel and band from the frequency; the channels summary and
 * the least crowded channel per band (Wi-Fi Survey's rule and its stub's lines); the sort; nmcli's command lines and
 * errors in plain words; the interface to scan with; the scan loop on a fake clock and a fake runner (the cadence: yes,
 * then auto; a manual rescan; a refused scan read from NetworkManager's last results; hard and soft failures; Wi-Fi
 * switched off; cancel while nmcli runs and while waiting; the vendor lookup asked once per OUI); the session on its
 * thread, and on a fake nmcli program run by the real runner (Stop ends it); the detail's network followed by BSSID; the
 * saved text.
 */
#include "check.hpp"
#include "wifi_fakes.hpp"

#include "interfaces.hpp"
#include "netlog_wifi.hpp"
#include "scope.hpp"
#include "text.hpp"
#include "wifiscan.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <thread>

using namespace nt;

namespace {

std::string wifi_fixture(const char *name)
{
    return fixture("wifi", name);
}

bool has(const std::string &text, const std::string &part)
{
    return text.find(part) != std::string::npos;
}

template <typename F> bool eventually(F f, int ms)
{
    const std::int64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

const WifiNetwork *by_bssid(const std::vector<WifiNetwork> &list, const std::string &bssid)
{
    for (const WifiNetwork &w : list)
        if (w.bssid == bssid) return &w;
    return nullptr;
}

void test_terse_and_ssid()
{
    // Wi-Fi Survey's own test of the splitter
    const std::vector<std::string> f = split_terse("*:AA\\:BB:Home\\:Net:x");
    CHECK(f.size() == 4 && f[1] == "AA:BB" && f[2] == "Home:Net" && f[3] == "x");
    CHECK_EQ(split_terse("a\\\\b:c").size(), static_cast<std::size_t>(2));
    CHECK_EQ(split_terse("a\\\\b:c")[0], std::string("a\\b"));
    CHECK_EQ(split_terse("::").size(), static_cast<std::size_t>(3));
    CHECK_EQ(split_terse("end\\")[0], std::string("end\\")); /* a lone backslash at the end stays */
    CHECK_EQ(split_terse("").size(), static_cast<std::size_t>(1));

    // SSIDs: UTF-8 kept, anything else as '?'
    CHECK_EQ(clean_ssid("Caf\xc3\xa9"), std::string("Caf\xc3\xa9"));
    CHECK_EQ(clean_ssid("Caf\xe9"), std::string("Caf?"));                  /* Latin-1, not UTF-8 */
    CHECK_EQ(clean_ssid("a\x01" "b\tc\x7f"), std::string("a?b?c?"));
    CHECK_EQ(clean_ssid("\xc0\xaf"), std::string("??"));                    /* overlong '/' */
    CHECK_EQ(clean_ssid("\xed\xa0\x80"), std::string("???"));               /* a surrogate */
    CHECK_EQ(clean_ssid("\xf0\x9f\x93\xb6 net"), std::string("\xf0\x9f\x93\xb6 net")); /* an emoji */
    CHECK_EQ(clean_ssid("\xf5\x80\x80\x80"), std::string("????"));
    CHECK_EQ(clean_ssid("x\xc2\x85y"), std::string("x?y"));                 /* a C1 control */
    CHECK_EQ(clean_ssid("\xff\xfe"), std::string("??"));
    CHECK_EQ(clean_ssid("\xe2\x82"), std::string("??"));                    /* cut in the middle */
    CHECK_EQ(clean_ssid(std::string(300, 'a')).size(), static_cast<std::size_t>(96));
    std::string many;
    for (int i = 0; i < 60; ++i) many += "\xc3\xa9";
    const std::string cut = clean_ssid(many);
    CHECK(cut.size() == 96 && cut.substr(94) == "\xc3\xa9");               /* never in a sequence */

    // a line with an SSID that is not UTF-8, and one with a control byte
    WifiNetwork w;
    CHECK(parse_nmcli_wifi_line(" :28\\:6F\\:B9\\:00\\:00\\:10:Caf\xe9 \xff:Infra:1:2412 MHz:54 Mbit/s:50:WPA2", w));
    CHECK_EQ(w.ssid, std::string("Caf? ?"));
    CHECK(!w.hidden);
    CHECK(parse_nmcli_wifi_line(" :28\\:6F\\:B9\\:00\\:00\\:11:a\x1b[31mb:Infra:1:2412 MHz:54 Mbit/s:50:WPA2", w));
    CHECK_EQ(w.ssid, std::string("a?[31mb"));
}

void test_recorded_list()
{
    const std::vector<WifiNetwork> list = parse_nmcli_wifi_list(wifi_fixture("nmcli-3a-plus.txt"));
    CHECK_EQ(list.size(), static_cast<std::size_t>(4));
    if (list.size() != 4) return;
    const WifiNetwork &home = list[0];
    CHECK(home.in_use && !home.hidden && !home.local_bssid && !home.adhoc);
    CHECK_EQ(home.ssid, std::string("HomeNet"));
    CHECK_EQ(home.name(), std::string("HomeNet"));
    CHECK_EQ(home.bssid, std::string("b0:4e:26:00:00:01"));
    CHECK_EQ(home.channel, 9);
    CHECK_EQ(home.freq_mhz, 2452);
    CHECK(home.band == WifiBand::G24);
    CHECK_EQ(home.rate_mbit, 270);
    CHECK_EQ(home.signal_pct, 57);
    CHECK_EQ(home.dbm, -66);
    CHECK(home.security == WifiSecurity::Wpa2 && home.security_text == "WPA2" && home.security_raw == "WPA2");
    const WifiNetwork &shadow = list[1]; /* a hidden network on the same radio: a virtual BSSID */
    CHECK(shadow.hidden && shadow.ssid.empty() && shadow.local_bssid && !shadow.in_use);
    CHECK_EQ(shadow.name(), std::string("(hidden)"));
    CHECK_EQ(shadow.dbm, -67);
    CHECK_EQ(list[2].ssid, std::string("Neighbour-0002"));
    CHECK(list[2].channel == 1 && list[2].freq_mhz == 2412 && list[2].rate_mbit == 130 && list[2].dbm == -78);
    CHECK(list[3].hidden && list[3].band == WifiBand::G5 && list[3].channel == 36 && list[3].dbm == -81);

    // the dBm estimate against iw's reading of the same link (-65 dBm): NetworkManager's formula, not Wi-Fi Survey's
    const IwLink link = parse_iw_link(wifi_fixture("iw-link-3a-plus.txt"));
    CHECK(link.connected && link.has_signal && link.signal_dbm == -65 && link.bssid == home.bssid);
    CHECK(std::abs(home.dbm - link.signal_dbm) <= 2);
    CHECK(std::abs((home.signal_pct / 2 - 100) - link.signal_dbm) > 5); /* pct / 2 - 100 = -72: off by 7 dB */
    CHECK(std::abs(nm_dbm_to_signal(link.signal_dbm) - home.signal_pct) <= 3);

    // sorted already (strongest first); in use first among equals
    std::vector<WifiNetwork> sorted = list;
    sort_networks(sorted);
    CHECK(sorted[0].bssid == home.bssid && sorted[3].band == WifiBand::G5);

    // the channels: two networks on 9 and one on 1; one on 5 GHz channel 36; nothing on 6 GHz
    const ChannelSummary s = summarize_channels(list);
    CHECK_EQ(s.g24.networks, 3);
    CHECK_EQ(s.g24.channels.size(), static_cast<std::size_t>(13));
    CHECK(s.g24.channels[8].channel == 9 && s.g24.channels[8].networks == 2 && s.g24.channels[8].weight == 10);
    CHECK(s.g24.channels[0].networks == 1 && s.g24.channels[0].weight == 5);
    CHECK_EQ(s.g24.channels[3].weight, 2); /* channel 4: 2 from channel 1, nothing from 9 */
    CHECK_EQ(s.g24.recommended, 4);        /* the lowest weight (2, also channel 13): the first found */
    CHECK_EQ(s.g5.networks, 1);
    CHECK_EQ(s.g5.channels.size(), static_cast<std::size_t>(9));
    CHECK_EQ(s.g5.recommended, 40);
    CHECK(s.g6.networks == 0 && s.g6.channels.empty() && s.g6.recommended == 0);
}

void test_made_up_list()
{
    const std::vector<WifiNetwork> list = parse_nmcli_wifi_list(wifi_fixture("nmcli-made-up.txt"));
    CHECK_EQ(list.size(), static_cast<std::size_t>(11));
    const WifiNetwork *home = by_bssid(list, "aa:bb:cc:00:00:01");
    CHECK(home && home->ssid == "Home:Net" && home->in_use && home->signal_pct == 82); /* the weaker repeat dropped */
    CHECK(home && home->local_bssid);                                                  /* 0xAA: bit 1 set */
    const WifiNetwork *mixed = by_bssid(list, "02:11:22:00:00:02");
    CHECK(mixed && mixed->hidden && mixed->security_text == "WPA2/WPA3" && mixed->security == WifiSecurity::Wpa2);
    const WifiNetwork *cafe = by_bssid(list, "b8:27:eb:00:00:03");
    CHECK(cafe && cafe->security == WifiSecurity::Open && cafe->security_text == "Open" && security_weak(cafe->security));
    CHECK(cafe && cafe->security_raw.empty() && !cafe->local_bssid);
    const WifiNetwork *office1 = by_bssid(list, "dc:a6:32:00:00:04");
    const WifiNetwork *office2 = by_bssid(list, "dc:a6:32:00:00:05");
    CHECK(office1 && office2 && office1->ssid == "Office" && office2->ssid == "Office"); /* one SSID, two BSSIDs */
    CHECK(office1 && office1->band == WifiBand::G5 && office1->channel == 149 && office1->freq_mhz == 5745);
    CHECK(office2 && office2->channel == 36 && office2->rate_mbit == 540);
    CHECK(office1 && office1->security == WifiSecurity::Enterprise && office1->security_text == "Enterprise");
    const WifiNetwork *six = by_bssid(list, "28:6f:b9:00:00:06");
    CHECK(six && six->band == WifiBand::G6 && six->channel == 37 && six->freq_mhz == 6135 && six->rate_mbit == 1201);
    CHECK(six && six->security == WifiSecurity::Wpa3 && six->security_text == "WPA3");
    const WifiNetwork *slash = by_bssid(list, "28:6f:b9:00:00:07");
    CHECK(slash && slash->ssid == "Back\\slash" && slash->security == WifiSecurity::Wpa && slash->security_text == "WPA/WPA2");
    const WifiNetwork *old = by_bssid(list, "28:6f:b9:00:00:08");
    CHECK(old && old->adhoc && old->security == WifiSecurity::Wep && security_weak(old->security) && old->channel == 3);
    const WifiNetwork *guest = by_bssid(list, "28:6f:b9:00:00:09");
    CHECK(guest && guest->security == WifiSecurity::EnhancedOpen && guest->security_text == "Enhanced Open");
    CHECK(guest && !security_weak(guest->security));
    const WifiNetwork *dashes = by_bssid(list, "28:6f:b9:00:00:0a");
    CHECK(dashes && dashes->hidden && dashes->security == WifiSecurity::Open && dashes->security_raw.empty());
    const WifiNetwork *colon = by_bssid(list, "28:6f:b9:00:00:0b");
    CHECK(colon && colon->ssid == "Colon:in" && colon->channel == 1 && colon->signal_pct == 5 && colon->security_text == "WPA2");
    CHECK(!by_bssid(list, "28:6f:b9:00:00:0d") && !by_bssid(list, "28:6f:b9:00:00:0e")); /* no frequency, too short */

    // sorted: strongest first
    std::vector<WifiNetwork> sorted = list;
    sort_networks(sorted);
    CHECK(sorted.front().bssid == "aa:bb:cc:00:00:01" && sorted.back().bssid == "28:6f:b9:00:00:0b");
    bool descending = true;
    for (std::size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].signal_pct > sorted[i - 1].signal_pct) descending = false;
    CHECK(descending);
    CHECK(sorted[1].bssid == "dc:a6:32:00:00:05" && sorted[2].bssid == "28:6f:b9:00:00:09");

    // the channels of all three bands
    const ChannelSummary s = summarize_channels(list);
    CHECK_EQ(s.g24.networks, 7);
    CHECK_EQ(s.g24.recommended, 11); /* weights 8 on 8 to 13: the preferred 11 wins the tie */
    CHECK(s.g24.channels[0].networks == 2 && s.g24.channels[5].networks == 2 && s.g24.channels[12].networks == 1);
    CHECK_EQ(s.g5.networks, 3);
    CHECK_EQ(s.g5.recommended, 40);
    CHECK_EQ(s.g6.networks, 1);
    CHECK_EQ(s.g6.recommended, 5);
    CHECK_EQ(s.g6.channels.size(), static_cast<std::size_t>(15)); /* 37 is a preferred scanning channel already */

    // Wi-Fi Survey's stub lines
    const std::vector<WifiNetwork> stub = parse_nmcli_wifi_list(
        "*:AA\\:BB\\:CC\\:DD\\:EE\\:01:Home\\:Net:Infra:6:2437 MHz:130 Mbit/s:82:WPA2\n"
        " :02\\:11\\:22\\:33\\:44\\:55::Infra:11:2462 MHz:54 Mbit/s:40:WPA2 WPA3\n"
        " :B8\\:27\\:EB\\:00\\:00\\:01:Cafe:Infra:1:2412 MHz:54 Mbit/s:30:--\n"
        " :DC\\:A6\\:32\\:00\\:00\\:02:Five:Infra:149:5745 MHz:270 Mbit/s:55:WPA2 802.1X\n");
    CHECK_EQ(stub.size(), static_cast<std::size_t>(4));
    if (stub.size() == 4) {
        CHECK(stub[0].in_use && stub[0].ssid == "Home:Net" && stub[0].bssid == "aa:bb:cc:dd:ee:01" && stub[0].channel == 6);
        CHECK(stub[1].hidden && stub[1].security_text == "WPA2/WPA3");
        CHECK(stub[2].security_text == "Open" && stub[2].channel == 1);
        CHECK(stub[3].band == WifiBand::G5 && stub[3].security_text == "Enterprise");
    }
    CHECK(parse_nmcli_wifi_list("").empty());
    CHECK(parse_nmcli_wifi_list(wifi_fixture("nmcli-notfound-3a-plus.txt")).empty());

    // at most kWifiMaxNetworks
    std::string big;
    for (int i = 0; i < 300; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), " :28\\:6F\\:B9\\:00\\:%02X\\:%02X:n%d:Infra:1:2412 MHz:54 Mbit/s:50:WPA2\n",
                      i / 256, i % 256, i);
        big += line;
    }
    CHECK_EQ(parse_nmcli_wifi_list(big).size(), kWifiMaxNetworks);
}

void test_numbers()
{
    CHECK_EQ(nm_signal_to_dbm(100), -40);
    CHECK_EQ(nm_signal_to_dbm(0), -100);
    CHECK_EQ(nm_signal_to_dbm(57), -66);
    CHECK_EQ(nm_signal_to_dbm(50), -70);
    CHECK_EQ(nm_signal_to_dbm(150), -40);
    CHECK_EQ(nm_signal_to_dbm(-5), -100);
    CHECK_EQ(nm_dbm_to_signal(-30), 100);
    CHECK_EQ(nm_dbm_to_signal(-110), 0);
    CHECK_EQ(nm_dbm_to_signal(-66), 57);
    bool round_trip = true;
    for (int d = -100; d <= -40; ++d)
        if (nm_signal_to_dbm(nm_dbm_to_signal(d)) != d) round_trip = false;
    CHECK(round_trip);

    CHECK_EQ(channel_of_freq(2412), 1);
    CHECK_EQ(channel_of_freq(2437), 6);
    CHECK_EQ(channel_of_freq(2472), 13);
    CHECK_EQ(channel_of_freq(2484), 14);
    CHECK_EQ(channel_of_freq(5180), 36);
    CHECK_EQ(channel_of_freq(5745), 149);
    CHECK_EQ(channel_of_freq(5825), 165);
    CHECK_EQ(channel_of_freq(5935), 2);
    CHECK_EQ(channel_of_freq(5955), 1);
    CHECK_EQ(channel_of_freq(6135), 37);
    CHECK_EQ(channel_of_freq(7115), 233);
    CHECK_EQ(channel_of_freq(4920), 184);
    CHECK_EQ(channel_of_freq(2400), 0);
    CHECK_EQ(channel_of_freq(0), 0);
    CHECK(band_of_freq(2412) == WifiBand::G24 && band_of_freq(2484) == WifiBand::G24);
    CHECK(band_of_freq(5180) == WifiBand::G5 && band_of_freq(5885) == WifiBand::G5 && band_of_freq(4920) == WifiBand::G5);
    CHECK(band_of_freq(5935) == WifiBand::G6 && band_of_freq(7115) == WifiBand::G6);
    CHECK(band_of_freq(900) == WifiBand::Unknown && band_of_freq(60000) == WifiBand::Unknown);
    CHECK_EQ(std::string(band_words(WifiBand::G24)), std::string("2.4 GHz"));
    CHECK_EQ(std::string(band_words(WifiBand::G6)), std::string("6 GHz"));

    CHECK_EQ(security_words(""), std::string("Open"));
    CHECK_EQ(security_words("--"), std::string("Open"));
    CHECK_EQ(security_words("WEP"), std::string("WEP"));
    CHECK_EQ(security_words("WPA1"), std::string("WPA"));
    CHECK_EQ(security_words("WPA1 WPA2"), std::string("WPA/WPA2"));
    CHECK_EQ(security_words("WPA1 WPA2 WPA3"), std::string("WPA/WPA2/WPA3"));
    CHECK_EQ(security_words("WPA2"), std::string("WPA2"));
    CHECK_EQ(security_words("WPA2 WPA3"), std::string("WPA2/WPA3"));
    CHECK_EQ(security_words("WPA3"), std::string("WPA3"));
    CHECK_EQ(security_words("WPA3 802.1X"), std::string("Enterprise"));
    CHECK_EQ(security_words("802.1X"), std::string("Enterprise"));
    CHECK_EQ(security_words("OWE"), std::string("Enhanced Open"));
    CHECK_EQ(security_words("FUTURE"), std::string("FUTURE"));
    CHECK(security_class("WPA1 WPA2") == WifiSecurity::Wpa && security_class("WPA2 WPA3") == WifiSecurity::Wpa2);
    CHECK(security_weak(WifiSecurity::Open) && security_weak(WifiSecurity::Wep) && !security_weak(WifiSecurity::Wpa));

    CHECK(valid_bssid("b0:4e:26:00:00:01") && valid_bssid("B0:4E:26:00:00:01"));
    CHECK(!valid_bssid("b0:4e:26:00:00") && !valid_bssid("b0-4e-26-00-00-01") && !valid_bssid("zz:4e:26:00:00:01"));
    CHECK(bssid_is_local("b6:4e:26:00:00:01") && bssid_is_local("02:00:00:00:00:00") && !bssid_is_local("b0:4e:26:00:00:01"));
}

void test_sort_and_channels()
{
    const auto net = [](const char *ssid, const char *bssid, int pct, bool in_use, int freq) {
        WifiNetwork w;
        w.ssid = ssid;
        w.hidden = !*ssid;
        w.bssid = bssid;
        w.signal_pct = pct;
        w.in_use = in_use;
        w.freq_mhz = freq;
        w.band = band_of_freq(freq);
        w.channel = channel_of_freq(freq);
        return w;
    };
    std::vector<WifiNetwork> v = {net("b", "00:00:00:00:00:05", 50, false, 2412), net("", "00:00:00:00:00:04", 50, false, 2412),
                                  net("a", "00:00:00:00:00:03", 50, false, 2412), net("z", "00:00:00:00:00:02", 50, true, 2412),
                                  net("a", "00:00:00:00:00:01", 50, false, 2412), net("w", "00:00:00:00:00:06", 90, false, 2412)};
    sort_networks(v);
    CHECK_EQ(v[0].ssid, std::string("w"));                     /* strongest */
    CHECK_EQ(v[1].ssid, std::string("z"));                     /* in use, among equals */
    CHECK(v[2].ssid == "a" && v[2].bssid == "00:00:00:00:00:01"); /* then by name, then BSSID */
    CHECK(v[3].ssid == "a" && v[3].bssid == "00:00:00:00:00:03");
    CHECK_EQ(v[4].ssid, std::string("b"));
    CHECK(v[5].hidden);                                        /* hidden last */

    // Wi-Fi Survey's 2.4 GHz rule
    CHECK_EQ(least_crowded_24({}), 0);
    CHECK_EQ(least_crowded_24({net("a", "00:00:00:00:00:01", 50, false, 2412)}), 6);   /* only on 1 */
    CHECK_EQ(least_crowded_24({net("a", "00:00:00:00:00:01", 50, false, 2437)}), 1);   /* only on 6 */
    CHECK_EQ(least_crowded_24({net("a", "00:00:00:00:00:01", 50, false, 2412), net("b", "00:00:00:00:00:02", 50, false, 2437),
                               net("c", "00:00:00:00:00:03", 50, false, 2462)}),
             13);                                                                   /* 1, 6, 11 busy: 13 weighs 3 */
    CHECK_EQ(least_crowded_24({net("a", "00:00:00:00:00:01", 50, false, 5180)}), 1);   /* nothing on 2.4 GHz */
    const ChannelSummary only5 = summarize_channels({net("a", "00:00:00:00:00:01", 50, false, 5180),
                                                     net("b", "00:00:00:00:00:02", 50, false, 5180),
                                                     net("c", "00:00:00:00:00:03", 50, false, 5200)});
    CHECK(only5.g24.networks == 0 && only5.g24.recommended == 1 && only5.g24.channels.size() == 13);
    CHECK_EQ(only5.g5.recommended, 44);
    CHECK(only5.g5.channels[0].channel == 36 && only5.g5.channels[0].networks == 2 && only5.g5.channels[0].weight == 10);
    const ChannelSummary none = summarize_channels({});
    CHECK(none.g24.recommended == 0 && none.g5.recommended == 0 && none.g6.recommended == 0);
    const ChannelSummary ch14 = summarize_channels({net("j", "00:00:00:00:00:01", 50, false, 2484)});
    CHECK(ch14.g24.channels.size() == 14 && ch14.g24.channels[13].networks == 1);
    const ChannelSummary dfs = summarize_channels({net("d", "00:00:00:00:00:01", 50, false, 5500)}); /* channel 100 */
    CHECK(dfs.g5.channels.size() == 10 && dfs.g5.recommended == 36);
}

void test_nmcli_words()
{
    const std::vector<std::string> a = nmcli_wifi_argv("wlan1", "yes");
    const std::vector<std::string> want = {"nmcli", "-t", "-e", "yes", "-f", "IN-USE,BSSID,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY",
                                           "device", "wifi", "list", "ifname", "wlan1", "--rescan", "yes"};
    CHECK(a == want);
    const std::vector<std::string> b = nmcli_wifi_argv("", "bogus");
    CHECK(b.size() == 11 && b[9] == "--rescan" && b[10] == "auto");
    CHECK(nmcli_wifi_argv("wlan0", "no").back() == "no");
    CHECK((nmcli_radio_argv() == std::vector<std::string>{"nmcli", "-t", "-f", "WIFI-HW,WIFI", "radio"}));
    CHECK(nmcli_radio_enabled("enabled:enabled\n"));
    CHECK(!nmcli_radio_enabled("enabled:disabled\n") && !nmcli_radio_enabled("disabled:enabled\n"));

    const CommandResult notfound = wfake::result(10, wifi_fixture("nmcli-notfound-3a-plus.txt"));
    CHECK(classify_nmcli(notfound) == NmcliFailure::NoAdapter);
    CHECK(classify_nmcli(wfake::result(10, wifi_fixture("nmcli-not-wifi-3a-plus.txt"))) == NmcliFailure::NoAdapter);
    CHECK_EQ(nmcli_failure_words(NmcliFailure::NoAdapter, notfound, "wlan1"), std::string("No Wi-Fi adapter on wlan1"));
    CHECK_EQ(nmcli_failure_words(NmcliFailure::NoAdapter, notfound, ""), std::string("No Wi-Fi adapter on this board"));
    const CommandResult down = wfake::result(8, "Error: NetworkManager is not running.\n");
    CHECK(classify_nmcli(down) == NmcliFailure::NotRunning);
    CHECK(has(nmcli_failure_words(NmcliFailure::NotRunning, down, "wlan0"), "NetworkManager is not running"));
    CHECK(classify_nmcli(wfake::not_installed()) == NmcliFailure::Missing);
    CHECK(classify_nmcli(wfake::result(127, "")) == NmcliFailure::Missing);
    CHECK(has(nmcli_failure_words(NmcliFailure::Missing, wfake::not_installed(), "wlan0"), "network-manager"));
    CommandResult slow = wfake::result(-1, "");
    slow.timed_out = true;
    CHECK(classify_nmcli(slow) == NmcliFailure::TimedOut);
    CommandResult gone = wfake::result(-1, "");
    gone.cancelled = true;
    CHECK(classify_nmcli(gone) == NmcliFailure::Cancelled);
    CHECK(classify_nmcli(wfake::result(0, wifi_fixture("nmcli-3a-plus.txt"))) == NmcliFailure::None);
    CHECK(classify_nmcli(wfake::result(1, "Error: Scanning not allowed immediately following previous scan.\n")) ==
          NmcliFailure::Refused);
    const CommandResult other = wfake::result(1, "\nError: something odd \x01 happened.\n");
    CHECK(classify_nmcli(other) == NmcliFailure::Other);
    CHECK_EQ(nmcli_failure_words(NmcliFailure::Other, other, "wlan0"),
             std::string("nmcli failed (exit 1): Error: something odd ? happened."));
}

void test_interface_choice()
{
    const auto state_of = [](const char *board) { return read_net_state(*make_fixture_source(fixture_dir(board))); };
    const std::unique_ptr<Source> dongle = make_fixture_source(fixture_dir("pi-3a-plus-dongle"));
    CHECK(iface_is_wireless(*dongle, "wlan0") && iface_is_wireless(*dongle, "wlan1"));
    CHECK(!iface_is_wireless(*dongle, "lo") && !iface_is_wireless(*dongle, "wlan9"));
    CHECK(!iface_is_wireless(*dongle, "../wlan0") && !iface_is_wireless(*dongle, "") && !iface_is_wireless(*dongle, "-x"));
    const std::unique_ptr<Source> eth = make_fixture_source(fixture_dir("eth-wifi"));
    CHECK(!iface_is_wireless(*eth, "end0") && iface_is_wireless(*eth, "wlan0"));

    const NetState pi5 = state_of("pi-5"); /* Ethernet is the main way out; Wi-Fi off */
    CHECK_EQ(wifi_scan_iface(resolve_scope("", pi5), pi5), std::string("wlan0"));
    const NetState two = state_of("pi-3a-plus-dongle");
    CHECK_EQ(wifi_scan_iface(resolve_scope("", two), two), std::string("wlan0"));
    CHECK_EQ(wifi_scan_iface(resolve_scope("wlan1", two), two), std::string("wlan1"));
    CHECK_EQ(wifi_scan_iface(resolve_scope("wlan7", two), two), std::string("wlan7")); /* missing: the scan says so */
    const NetState ew = state_of("eth-wifi");
    CHECK_EQ(wifi_scan_iface(resolve_scope("end0", ew), ew), std::string("end0"));    /* not Wi-Fi: the scan says so */
    CHECK_EQ(wifi_scan_iface(resolve_scope("", ew), ew), std::string("wlan0"));
    NetState wired;
    Iface lo;
    lo.name = "lo";
    lo.loopback = true;
    Iface e0;
    e0.name = "eth0";
    wired.ifaces = {e0, lo};
    wired.main_iface = "eth0";
    CHECK_EQ(wifi_scan_iface(resolve_scope("", wired), wired), std::string(""));
}

/* ---------------------------------------------------------------- the loop on a fake clock */

struct LoopRun {
    std::shared_ptr<wfake::Time> time = std::make_shared<wfake::Time>();
    wfake::Runner runner;
    std::unique_ptr<Source> src = make_fixture_source(fixture_dir("pi-3a-plus"));
    std::vector<WifiSnapshot> published;
    CancelToken cancel;
    std::atomic<bool> wake{false};
    WifiScanConfig cfg;
    LoopRun()
    {
        runner.time = time;
        cfg.iface = "wlan0";
    }
    WifiSnapshot go()
    {
        return run_wifi_scan(cfg, *src, runner, wfake::clock_of(time), cancel, &wake,
                             [this](const WifiSnapshot &s) { published.push_back(s); });
    }
};

void test_scan_loop()
{
    const std::string list = wifi_fixture("nmcli-3a-plus.txt");
    // the cadence: a fresh scan first, then "auto" every interval; vendors asked once per OUI
    {
        LoopRun t;
        int lookups = 0;
        std::map<std::string, std::string> table = {{"b0:4e:26", "TP-Link"}, {"c8:7f:2b", "Neighbour vendor"}};
        t.cfg.vendor = [&](const std::string &mac) {
            ++lookups;
            const auto it = table.find(mac.substr(0, 8));
            return it == table.end() ? std::string() : it->second;
        };
        t.cfg.max_lists = 4;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        const WifiSnapshot end = t.go();
        CHECK_EQ(t.runner.calls(), 4);
        CHECK(t.runner.rescan(0) == "yes" && t.runner.rescan(1) == "auto" && t.runner.rescan(3) == "auto");
        CHECK(t.runner.at(0) == 1000 && t.runner.at(1) == 6000 && t.runner.at(2) == 11000 && t.runner.at(3) == 16000);
        const CommandSpec spec = t.runner.spec(0);
        CHECK(spec.argv.size() == 13 && spec.argv[0] == "nmcli" && spec.argv[10] == "wlan0");
        CHECK_EQ(spec.timeout_ms, kNmcliScanTimeoutMs);
        CHECK(end.state == WifiSnapshot::State::Stopped && end.lists == 4 && !end.busy);
        CHECK_EQ(end.networks.size(), static_cast<std::size_t>(4));
        CHECK_EQ(lookups, 2); /* b0:4e:26 and c8:7f:2b, once each; the local BSSIDs are never asked */
        CHECK(end.networks[0].vendor == "TP-Link" && end.networks[1].vendor.empty() && end.networks[2].vendor == "Neighbour vendor");
        CHECK(end.channels.g24.recommended == 4 && end.channels.g5.recommended == 40);
        CHECK(!t.published.empty() && t.published[0].state == WifiSnapshot::State::Scanning && t.published[0].lists == 0);
        CHECK(t.published.size() >= 2 && t.published[1].busy && t.published[1].state == WifiSnapshot::State::Scanning);
        CHECK(t.published.size() >= 3 && t.published[2].state == WifiSnapshot::State::Ready && t.published[2].rescan == "yes");
        CHECK(t.published[2].list_at_ms == 1000 && !t.published[2].cached);
    }
    // a manual rescan: a fresh scan at once, then the cadence from there
    {
        LoopRun t;
        t.cfg.max_lists = 3;
        bool pressed = false;
        t.time->on_sleep = [&](std::int64_t now) {
            if (now >= 3000 && !pressed) {
                pressed = true;
                t.wake = true;
            }
        };
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        t.go();
        CHECK_EQ(t.runner.calls(), 3);
        CHECK(t.runner.rescan(0) == "yes" && t.runner.rescan(1) == "yes" && t.runner.rescan(2) == "auto");
        CHECK(t.runner.at(1) == 3000 && t.runner.at(2) == 8000);
        CHECK(!t.wake.load());
    }
    // a refused fresh scan: NetworkManager's last results, said so; then "auto"
    {
        LoopRun t;
        t.cfg.max_lists = 2;
        t.runner.answer = [&](const CommandSpec &, int i) {
            if (i == 0) return wfake::result(1, "Error: Scanning not allowed immediately following previous scan.\n");
            return wfake::result(0, list);
        };
        const WifiSnapshot end = t.go();
        CHECK_EQ(t.runner.calls(), 3);
        CHECK(t.runner.rescan(0) == "yes" && t.runner.rescan(1) == "no" && t.runner.rescan(2) == "auto");
        bool saw_cached = false;
        for (const WifiSnapshot &s : t.published)
            if (s.state == WifiSnapshot::State::Ready && s.cached && s.rescan == "no" && s.lists == 1) saw_cached = true;
        CHECK(saw_cached);
        CHECK(!end.cached && end.rescan == "auto" && end.lists == 2);
    }
    // hard failures: no adapter as NetworkManager sees it, no nmcli, no NetworkManager (each round asks a fresh scan)
    {
        LoopRun t;
        t.cfg.max_lists = 2;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(10, wifi_fixture("nmcli-notfound-3a-plus.txt")); };
        const WifiSnapshot end = t.go();
        CHECK(t.runner.calls() == 2 && t.runner.rescan(1) == "yes");
        CHECK(end.networks.empty() && end.error == "No Wi-Fi adapter on wlan0");
        CHECK(t.published.size() >= 3 && t.published[2].state == WifiSnapshot::State::Failed);
    }
    {
        LoopRun t;
        t.cfg.max_lists = 1;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::not_installed(); };
        const WifiSnapshot end = t.go();
        CHECK(t.runner.calls() == 1 && has(end.error, "nmcli is missing"));
    }
    {
        LoopRun t;
        t.cfg.max_lists = 1;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(8, "Error: NetworkManager is not running.\n"); };
        CHECK(has(t.go().error, "not running"));
    }
    // the interface is not a Wi-Fi adapter, is missing, or there is none: nmcli is not even asked
    {
        LoopRun t;
        t.src = make_fixture_source(fixture_dir("eth-wifi"));
        t.cfg.iface = "end0";
        t.cfg.max_lists = 1;
        const WifiSnapshot end = t.go();
        CHECK(t.runner.calls() == 0 && end.error == "No Wi-Fi adapter on end0");
        CHECK(t.published.size() >= 2 && t.published[1].state == WifiSnapshot::State::Failed);
        LoopRun m;
        m.cfg.iface = "wlan5";
        m.cfg.max_lists = 1;
        CHECK_EQ(m.go().error, std::string("No Wi-Fi adapter on wlan5"));
        LoopRun n;
        n.cfg.iface = "";
        n.cfg.max_lists = 1;
        CHECK_EQ(n.go().error, std::string("No Wi-Fi adapter on this board"));
    }
    // a soft failure keeps the last list (and says why); the next list clears it
    {
        LoopRun t;
        t.cfg.max_lists = 3;
        t.runner.answer = [&](const CommandSpec &, int i) {
            if (i == 1) {
                CommandResult r = wfake::result(-1, "");
                r.timed_out = true;
                return r;
            }
            return wfake::result(0, list);
        };
        t.go();
        bool kept = false;
        for (const WifiSnapshot &s : t.published)
            if (s.state == WifiSnapshot::State::Ready && !s.busy && s.error == "NetworkManager did not answer in time." &&
                s.networks.size() == 4 && s.lists == 1)
                kept = true;
        CHECK(kept);
        CHECK(t.published.back().error.empty() && t.published.back().lists == 2);
    }
    // Wi-Fi switched off: an empty list, then the radio's answer
    {
        LoopRun t;
        t.cfg.max_lists = 1;
        t.runner.answer = [&](const CommandSpec &, int i) {
            return i == 0 ? wfake::result(0, "") : wfake::result(0, "enabled:disabled\n");
        };
        const WifiSnapshot end = t.go();
        CHECK(t.runner.calls() == 2 && t.runner.spec(1).argv == nmcli_radio_argv());
        CHECK_EQ(end.error, std::string("Wi-Fi is switched off."));
        LoopRun on;
        on.cfg.max_lists = 1;
        on.runner.answer = [&](const CommandSpec &, int i) {
            return i == 0 ? wfake::result(0, "") : wfake::result(0, "enabled:enabled\n");
        };
        const WifiSnapshot empty = on.go();
        CHECK(empty.error.empty() && empty.lists == 1 && empty.networks.empty() && empty.channels.g24.recommended == 0);
    }
    // cancel while nmcli runs, and while waiting for the next list
    {
        LoopRun t;
        t.runner.on_call = [&](int i) {
            if (i == 1) t.cancel.cancel();
        };
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        const WifiSnapshot end = t.go();
        CHECK_EQ(t.runner.calls(), 2);
        CHECK(end.state == WifiSnapshot::State::Stopped && end.networks.size() == 4 && !end.busy);
        LoopRun w;
        w.time->on_sleep = [&](std::int64_t now) {
            if (now >= 3000) w.cancel.cancel();
        };
        w.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        w.go();
        CHECK(w.runner.calls() == 1 && w.time->t.load() <= 3100);
    }
    // a program in front (tests, or a wrapper), and the interval kept within its bounds
    {
        LoopRun t;
        t.cfg.program = {"sh", "/fake/nmcli"};
        t.cfg.interval_ms = 10;
        t.cfg.max_lists = 2;
        t.runner.answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        const WifiSnapshot end = t.go();
        CHECK(t.runner.spec(0).argv[0] == "sh" && t.runner.spec(0).argv[1] == "/fake/nmcli" && t.runner.spec(0).argv[2] == "-t");
        CHECK(t.runner.at(1) - t.runner.at(0) == kWifiIntervalMinMs && end.interval_ms == kWifiIntervalMinMs);
    }
}

void test_scan_session()
{
    const std::string list = wifi_fixture("nmcli-3a-plus.txt");
    // on its thread, with a fast fake clock: lists come, a rescan asks a fresh scan, stop keeps the list
    {
        auto time = std::make_shared<wfake::Time>();
        time->real_us = 200;
        auto runner = std::make_shared<wfake::Runner>();
        runner->time = time;
        runner->answer = [&](const CommandSpec &, int) { return wfake::result(0, list); };
        std::shared_ptr<const Source> src = make_fixture_source(fixture_dir("pi-3a-plus"));
        WifiScanSession session(src, runner, wfake::clock_of(time));
        WifiScanConfig cfg;
        cfg.iface = "wlan0";
        cfg.interval_ms = 2000;
        CHECK(session.start(cfg));
        CHECK(session.latest() && session.latest()->running());
        CHECK(eventually([&] { return session.latest()->lists >= 2; }, 5000));
        const int before = runner->calls();
        session.rescan();
        CHECK(eventually([&] {
            for (int i = before; i < runner->calls(); ++i)
                if (runner->rescan(i) == "yes") return true;
            return false;
        }, 5000));
        session.stop();
        CHECK(session.wait(2000));
        const auto last = session.latest();
        CHECK(last->state == WifiSnapshot::State::Stopped && last->networks.size() == 4);
        // a new start begins again with a fresh scan
        const int calls = runner->calls();
        CHECK(session.start(cfg));
        CHECK(eventually([&] { return runner->calls() > calls; }, 5000));
        CHECK_EQ(runner->rescan(calls), std::string("yes"));
    } /* the destructor stops and joins the second run */

    // a fake nmcli run by the real runner: Stop ends it while it scans
    {
        const std::string dir = test_tmpdir("wifinmcli");
        const std::string script = dir + "/nmcli";
        std::ofstream(script) << "case \"$*\" in\n"
                                 "*\"--rescan yes\"*) cat '"
                              << fixture_dir("wifi") << "/nmcli-3a-plus.txt' ;;\n"
                                 "*\"--rescan auto\"*) exec sleep 30 ;;\n"
                                 "*radio*) echo enabled:enabled ;;\n"
                                 "esac\n";
        auto time = std::make_shared<wfake::Time>();
        time->real_us = 200;
        std::shared_ptr<const Source> src = make_fixture_source(fixture_dir("pi-3a-plus"));
        WifiScanSession session(src, std::make_shared<DirectRunner>(), wfake::clock_of(time));
        WifiScanConfig cfg;
        cfg.iface = "wlan0";
        cfg.interval_ms = 2000;
        cfg.program = {"sh", script};
        CHECK(session.start(cfg));
        CHECK(eventually([&] {
            const auto s = session.latest();
            return s->lists >= 1 && s->busy && s->state == WifiSnapshot::State::Ready;
        }, 8000));
        CHECK_EQ(session.latest()->networks.size(), static_cast<std::size_t>(4));
        CHECK(session.latest()->networks[0].ssid == "HomeNet");
        const std::int64_t t0 = now_ms();
        session.stop();
        CHECK(session.wait(3000));
        CHECK(now_ms() - t0 < 1500);
        CHECK(session.latest()->state == WifiSnapshot::State::Stopped && session.latest()->networks.size() == 4);
        // nmcli that is not there
        cfg.program = {dir + "/missing-nmcli"};
        cfg.max_lists = 1;
        CHECK(session.start(cfg));
        CHECK(session.wait(5000));
        CHECK(has(session.latest()->error, "nmcli is missing"));
        remove_dir(dir);
    }
}

void test_follower()
{
    const auto net = [](const char *bssid, int pct, const char *ssid) {
        WifiNetwork w;
        w.bssid = bssid;
        w.signal_pct = pct;
        w.dbm = nm_signal_to_dbm(pct);
        w.ssid = ssid;
        return w;
    };
    const auto snap = [](std::uint64_t lists, std::int64_t at, std::vector<WifiNetwork> v) {
        WifiSnapshot s;
        s.state = WifiSnapshot::State::Ready;
        s.lists = lists;
        s.list_at_ms = at;
        s.networks = std::move(v);
        return s;
    };
    const WifiNetwork a = net("00:00:00:00:00:0a", 80, "A"), b = net("00:00:00:00:00:0b", 60, "B"),
                      c = net("00:00:00:00:00:0c", 40, "C");
    WifiFollower f;
    CHECK(!f.active());
    f.update(snap(1, 100, {a, b, c})); /* not following: nothing */
    CHECK(!f.active());
    f.follow(b);
    CHECK(f.active() && !f.gone() && f.network().ssid == "B");
    f.update(snap(1, 100, {a, b, c}));
    CHECK(!f.gone() && f.network().signal_pct == 60);
    // reordered, new values
    const WifiSnapshot s2 = snap(2, 200, {net("00:00:00:00:00:0b", 90, "B"), a, c});
    f.update(s2);
    CHECK(!f.gone() && f.network().signal_pct == 90);
    CHECK_EQ(WifiFollower::index_in(s2, "00:00:00:00:00:0b"), 0);
    CHECK_EQ(WifiFollower::index_in(s2, "00:00:00:00:00:ff"), -1);
    // gone: the last values stay
    const WifiSnapshot s3 = snap(3, 300, {a, c});
    f.update(s3);
    CHECK(f.gone() && f.gone_lists() == 1 && f.network().signal_pct == 90 && f.network().ssid == "B");
    f.update(s3); /* the same list again: no change */
    CHECK_EQ(f.gone_lists(), 1);
    f.update(snap(4, 400, {a}));
    CHECK_EQ(f.gone_lists(), 2);
    // a failed scan (no new list) changes nothing
    WifiSnapshot failed = snap(4, 400, {});
    failed.state = WifiSnapshot::State::Failed;
    failed.error = "NetworkManager did not answer in time.";
    f.update(failed);
    CHECK(f.gone() && f.gone_lists() == 2);
    // back
    f.update(snap(5, 500, {a, net("00:00:00:00:00:0b", 30, "B")}));
    CHECK(!f.gone() && f.gone_lists() == 0 && f.network().signal_pct == 30);
    // a new run counts from 1 again: still a new list
    f.update(snap(1, 900, {a}));
    CHECK(f.gone());
    f.clear();
    CHECK(!f.active() && !f.gone());
    f.update(snap(0, 0, {}));
    CHECK(!f.active());
}

void test_wifi_text()
{
    WifiSnapshot s;
    s.state = WifiSnapshot::State::Ready;
    s.iface = "wlan0";
    s.networks = parse_nmcli_wifi_list(wifi_fixture("nmcli-3a-plus.txt"));
    s.networks[0].vendor = "TP-Link";
    s.networks[2].vendor = "Neighbour vendor";
    s.channels = summarize_channels(s.networks);
    s.lists = 3;
    const std::string text = format_wifi_networks(s);
    CHECK(text.find("Interface: wlan0\nState: ready, 4 networks\n") == 0);
    CHECK(has(text, "Name  BSSID  Signal  Channel  Band  Bit rate  Security  Vendor  Notes\n"));
    CHECK(has(text, "HomeNet  b0:4e:26:00:00:01  57% (about -66 dBm)  9  2.4 GHz  270 Mbit/s  WPA2  TP-Link  in use\n"));
    CHECK(has(text, "(hidden)  b6:4e:26:00:00:01  55% (about -67 dBm)  9  2.4 GHz  270 Mbit/s  WPA2  -  local address\n"));
    CHECK(has(text, "Channels:\n  2.4 GHz: 3 networks; channel 1: 1, channel 9: 2; least crowded: channel 4\n"
                    "  5 GHz: 1 network; channel 36: 1; least crowded: channel 40\n  6 GHz: none seen\n"));
    CHECK(!has(text, "Error:") && !has(text, "Note:"));
    s.cached = true;
    s.error = "NetworkManager did not answer in time.";
    const std::string t2 = format_wifi_networks(s);
    CHECK(has(t2, "Error: NetworkManager did not answer in time.\n") && has(t2, "Note: NetworkManager's last results"));
    WifiSnapshot failed;
    failed.state = WifiSnapshot::State::Failed;
    failed.iface = "end0";
    failed.error = "No Wi-Fi adapter on end0";
    CHECK_EQ(format_wifi_networks(failed), std::string("Interface: end0\nState: failed\nError: No Wi-Fi adapter on end0\n"));

    const std::string one = format_wifi_network(s.networks[1], true);
    CHECK(one.find("Network: (hidden) (gone: not in the last scan; its last values)\n") == 0);
    CHECK(has(one, "BSSID: b6:4e:26:00:00:01 (locally administered)\nVendor: unknown\nSignal: 55% (about -67 dBm)\n"));
    CHECK(has(one, "Channel: 9 (2452 MHz, 2.4 GHz)\nSecurity: WPA2\nBit rate: 270 Mbit/s\nMode: access point\nIn use: no\n"));
    WifiNetwork odd = parse_nmcli_wifi_list(wifi_fixture("nmcli-made-up.txt"))[3];
    CHECK(has(format_wifi_network(odd, false), "Security: Enterprise (WPA2 802.1X)\n"));
}

} // namespace

void test_wifiscan()
{
    test_terse_and_ssid();
    test_recorded_list();
    test_made_up_list();
    test_numbers();
    test_sort_and_channels();
    test_nmcli_words();
    test_interface_choice();
    test_scan_loop();
    test_scan_session();
    test_follower();
    test_wifi_text();
}
