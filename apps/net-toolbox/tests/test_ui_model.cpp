/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests: what the pages say (src/ui/view_model.cpp, compiled in here: it has no LVGL): the Wi-Fi signal words,
 * the interface rows on the recorded boards, the ladder's words and message, the ping scale / cells / statistics, the
 * DNS status and rows, the port check summary.
 */
#include "check.hpp"

#include "../src/ui/view_model.cpp"
#include "../src/ui/view_model_02.cpp"

using namespace nt;
using namespace nt::ui;

namespace {

NetState board(const char *name, bool with_iw)
{
    const auto src = make_fixture_source(fixture_dir(name));
    NetState st = read_net_state(*src);
    if (with_iw)
        for (const Iface &i : st.ifaces)
            if (i.wireless && i.connected()) apply_iw_link(st, i.name, parse_iw_link(fixture(name, "cmd/iw_dev_wlan0_link")));
    return st;
}

const RowDef *find_row(const std::vector<RowDef> &rows, const std::string &label)
{
    for (const RowDef &r : rows)
        if (!r.section && r.label == label) return &r;
    return nullptr;
}

PortSnapshot ports(std::initializer_list<PortState> states)
{
    PortSnapshot s;
    s.state = PortSnapshot::State::Done;
    s.host = "example.com";
    s.address = "93.184.215.14";
    std::uint16_t port = 80;
    for (const PortState st : states) {
        PortResult r;
        r.port = port++;
        r.state = st;
        r.connect_ms = 20;
        s.results.push_back(r);
    }
    return s;
}

} // namespace

void test_ui_model()
{
    // the signal words: -50 / -60 / -70 are the lower edges of Excellent / Good / Fair
    CHECK_EQ(std::string(signal_word(-30)), "Excellent");
    CHECK_EQ(std::string(signal_word(-50)), "Excellent");
    CHECK_EQ(std::string(signal_word(-51)), "Good");
    CHECK_EQ(std::string(signal_word(-60)), "Good");
    CHECK_EQ(std::string(signal_word(-61)), "Fair");
    CHECK_EQ(std::string(signal_word(-70)), "Fair");
    CHECK_EQ(std::string(signal_word(-71)), "Weak");
    CHECK_EQ(signal_text(-58), std::string("-58 dBm, Good"));
    CHECK(signal_tone(-58) == Tone::Ok && signal_tone(-65) == Tone::Warn && signal_tone(-80) == Tone::Bad);

    // the graph scale and the ring
    CHECK_NEAR(nice_top_ms(0), 10, 1e-9);
    CHECK_NEAR(nice_top_ms(8), 10, 1e-9);
    CHECK_NEAR(nice_top_ms(12), 15, 1e-9);
    CHECK_NEAR(nice_top_ms(16), 20, 1e-9);
    CHECK_NEAR(nice_top_ms(243), 300, 1e-9);
    CHECK_NEAR(nice_top_ms(1000), 1500, 1e-9);
    CHECK_NEAR(nice_top_ms(2.5e6), 3e6, 1e-9);
    CHECK_EQ(ring_slot(0, 1), 119);
    CHECK_EQ(ring_slot(4, 5), 119);
    CHECK_EQ(ring_slot(0, 5), 115);
    CHECK_EQ(ring_slot(0, 120), 0);
    CHECK_EQ(ring_slot(119, 120), 119);
    CHECK_EQ(ring_slot(0, 130), -1); /* older than the ring */
    CHECK_EQ(ring_slot(10, 130), 0);
    CHECK_EQ(packet_cell(PacketState::Lost).height_pct, 100);
    CHECK(packet_cell(PacketState::Ok).height_pct < packet_cell(PacketState::Slow).height_pct);
    CHECK(packet_cell(PacketState::Slow).height_pct < packet_cell(PacketState::Lost).height_pct);
    CHECK(packet_cell(PacketState::Pending).height_pct < packet_cell(PacketState::Ok).height_pct);
    {
        std::vector<PacketResult> p(3);
        p[0].state = PacketState::Ok;
        p[0].rtt_ms = 12;
        p[1].state = PacketState::Lost;
        p[2].state = PacketState::Slow;
        p[2].rtt_ms = 240;
        CHECK_NEAR(max_rtt_ms(p), 240, 1e-9);
        PingSnapshot s;
        s.state = PingSnapshot::State::Running;
        s.recent = p;
        CHECK_EQ(ping_head(s).text, std::string("now 240 ms (slow)"));
        CHECK(ping_head(s).tone == Tone::Warn);
        s.recent.pop_back();
        s.recent[1].state = PacketState::Pending;
        CHECK_EQ(ping_head(s).text, std::string("now 12 ms"));
        s.recent[0].state = PacketState::Lost;
        CHECK(ping_head(s).tone == Tone::Bad);
        PingSnapshot idle;
        CHECK_EQ(ping_band(idle).text, std::string("Press Start to ping"));
        PingSnapshot failed;
        failed.state = PingSnapshot::State::Failed;
        failed.error = "x does not exist (NXDOMAIN)";
        CHECK(ping_band(failed).tone == Tone::Bad);
    }
    CHECK_EQ(ping_scale(50, 120), std::string("0-50 ms, last 120 packets"));
    {
        PingStats st;
        CHECK_EQ(ping_stats(st), std::string());
        for (int i = 0; i < 4; ++i) st.add_sent();
        st.add_reply(10);
        st.add_reply(14);
        st.add_lost();
        const std::string t = ping_stats(st);
        CHECK(t.find("sent 4") != std::string::npos && t.find("lost 1") != std::string::npos);
        CHECK(t.find("min 10 ms") != std::string::npos && t.find("max 14 ms") != std::string::npos);
        CHECK(t.find("jitter") != std::string::npos);
    }

    // Home and Interfaces on the recorded boards
    CHECK_EQ(home_band(board("deck", false)).text, std::string("wlan0: 192.168.68.121"));
    CHECK(home_band(board("deck", false)).tone == Tone::Ok);
    CHECK_EQ(home_band(board("deck-offline", false)).text, std::string("No network"));
    CHECK(home_band(board("deck-offline", false)).tone == Tone::Warn);
    {
        const std::vector<RowDef> rows = interface_rows(board("deck", true));
        CHECK(rows[0].section && rows[0].label == "wlan0");
        const RowDef *sig = find_row(rows, "Signal");
        CHECK(sig && sig->value == "-58 dBm, Good");
        CHECK(find_row(rows, "Network") && find_row(rows, "Network")->value == "DeckNet");
        CHECK(find_row(rows, "Bit rate"));
        CHECK(find_row(rows, "State") && find_row(rows, "State")->value == "Connected");
        CHECK(find_row(rows, "Gateway") && find_row(rows, "Gateway")->value == "192.168.68.1");
        const RowDef *dns1 = find_row(rows, "DNS server 1");
        CHECK(dns1 && dns1->value == "192.168.1.1 (asked first)");
        bool network_heading = false;
        for (const RowDef &r : rows) network_heading = network_heading || (r.section && r.label == "Network");
        CHECK(network_heading);
    }
    {
        const std::vector<RowDef> rows = interface_rows(board("deck-offline", true));
        CHECK(find_row(rows, "State") && find_row(rows, "State")->value == "Not connected");
        CHECK(!find_row(rows, "Signal"));
        CHECK(find_row(rows, "Gateway") && find_row(rows, "Gateway")->value == "none");
    }
    {
        const std::vector<RowDef> rows = interface_rows(board("pi-5", false));
        CHECK(rows[0].section);
        const RowDef *speed = find_row(rows, "Speed");
        CHECK(speed && !speed->value.empty());
        CHECK(!find_row(rows, "Bit rate")); /* Wi-Fi is off on that board */
    }
    {
        const std::vector<RowDef> rows = interface_rows(board("pi-3a-plus", true));
        CHECK(find_row(rows, "Signal") && find_row(rows, "Signal")->value == "-67 dBm, Fair");
        CHECK(find_row(rows, "IPv6"));
    }

    // the ladder
    {
        DiagStep s;
        CHECK_EQ(step_text(s), std::string("waiting"));
        s.state = StepState::Running;
        CHECK_EQ(step_text(s), std::string("checking..."));
        s.state = StepState::Skipped;
        CHECK_EQ(step_text(s), std::string("skipped"));
        s.state = StepState::Pass;
        s.detail = "3 ms";
        CHECK_EQ(step_text(s), std::string("3 ms"));
        s.state = StepState::Fail;
        s.detail.clear();
        CHECK_EQ(step_text(s), std::string("failed"));
        CHECK(step_tone(StepState::Pass) == Tone::Ok && step_tone(StepState::Fail) == Tone::Bad);
        CHECK(step_tone(StepState::Running) == Tone::Warn && step_tone(StepState::Pending) == Tone::Normal);
        DiagReport r;
        CHECK_EQ(diag_message(r).head, std::string("Checking your connection..."));
        r.state = DiagReport::State::Passed;
        r.explanation = "All good.";
        CHECK_EQ(diag_message(r).head, std::string("Everything works"));
        CHECK_EQ(diag_message(r).body, std::string("All good."));
        r.notes = {"a", "b"};
        CHECK_EQ(diag_message(r).body, std::string("- a\n- b"));
        r.state = DiagReport::State::Failed;
        r.failed = static_cast<int>(Step::Dns);
        r.explanation = "DNS does not answer.";
        CHECK_EQ(diag_message(r).head, std::string("DNS failed"));
        CHECK(diag_message(r).tone == Tone::Bad);
        CHECK_EQ(diag_message(r).body, std::string("DNS does not answer."));
    }

    // DNS
    {
        DnsSnapshot s;
        CHECK_EQ(dns_status(s).text, std::string("Type a name and press Look up"));
        CHECK(dns_rows(s, "").empty());
        s.state = DnsSnapshot::State::Done;
        s.question = "example.com";
        s.type = dns::kA;
        s.servers = {"192.168.1.1"};
        DnsResult r;
        r.server = "192.168.1.1";
        r.answered = true;
        r.rcode = dns::kNxDomain;
        r.rcode_name = "NXDOMAIN";
        r.rcode_words = "no such name";
        r.elapsed_ms = 12;
        s.results.push_back(r);
        CHECK_EQ(dns_status(s).text, std::string("No such name (NXDOMAIN), 12 ms, via 192.168.1.1"));
        CHECK(dns_status(s).tone == Tone::Warn);
        std::vector<RowDef> rows = dns_rows(s, "192.168.1.1");
        CHECK(rows[0].section && rows.size() == 2 && rows[1].label == "No records");
        s.results[0].answered = false;
        s.results[0].error = "timed out";
        s.results[0].elapsed_ms = 4000;
        CHECK_EQ(dns_status(s).text, std::string("No answer: timed out after 4000 ms, via 192.168.1.1"));
        CHECK(dns_status(s).tone == Tone::Bad);
        // answers: the name is shown only when it differs from the question
        s.results[0] = DnsResult();
        s.results[0].answered = true;
        s.results[0].rcode = 0;
        s.results[0].rcode_name = "NOERROR";
        s.results[0].rcode_words = "OK";
        dns::Record a;
        a.name = "www.example.com";
        a.type = dns::kCNAME;
        a.ttl = 3600;
        a.data = "example.com";
        s.results[0].answers.push_back(a);
        a.name = "example.com";
        a.type = dns::kA;
        a.ttl = 300;
        a.data = "93.184.215.14";
        s.results[0].answers.push_back(a);
        rows = dns_rows(s, "");
        CHECK_EQ(rows.size(), std::size_t(3));
        CHECK_EQ(rows[1].label, std::string("www.example.com  example.com"));
        CHECK_EQ(rows[2].label, std::string("93.184.215.14"));
        CHECK_EQ(rows[2].value, std::string("A  300 s"));
        // compare: a row per resolver, the ones not asked yet waiting
        DnsSnapshot c;
        c.state = DnsSnapshot::State::Running;
        c.question = "example.com";
        c.type = dns::kA;
        c.servers = {"192.168.1.1", "1.1.1.1", "8.8.8.8"};
        DnsResult one;
        one.server = "192.168.1.1";
        one.answered = true;
        one.rcode = 0;
        one.rcode_name = "NOERROR";
        one.elapsed_ms = 12;
        one.answers.push_back(a);
        c.results.push_back(one);
        rows = dns_rows(c, "192.168.1.1");
        CHECK_EQ(rows.size(), std::size_t(4));
        CHECK_EQ(rows[1].label, std::string("System 192.168.1.1"));
        CHECK_EQ(rows[1].value, std::string("12 ms, 1 answer"));
        CHECK_EQ(rows[2].value, std::string("asking..."));
        CHECK_EQ(rows[3].value, std::string("waiting"));
        CHECK_EQ(rows[3].label, std::string("Google 8.8.8.8"));
        CHECK_EQ(dns_status(c).text, std::string("Asking resolver 2 of 3..."));
        c.state = DnsSnapshot::State::Done;
        c.results.push_back(one);
        c.results.back().server = "1.1.1.1";
        c.results.back().elapsed_ms = 7;
        c.results.push_back(one);
        c.results.back().server = "8.8.8.8";
        c.results.back().elapsed_ms = 20;
        CHECK_EQ(dns_status(c).text, std::string("Compared 3 resolvers, fastest 1.1.1.1 7 ms"));
        CHECK_EQ(resolver_label("9.9.9.9", ""), std::string("Quad9 9.9.9.9"));
        CHECK_EQ(resolver_label("10.0.0.1", ""), std::string("10.0.0.1"));
    }

    // Port check
    {
        PortSummary p = port_summary(ports({PortState::Open}));
        CHECK_EQ(p.word, std::string("Open"));
        CHECK(p.tone == Tone::Ok && p.mark == Mark::Check);
        CHECK_EQ(p.host, std::string("example.com (93.184.215.14)"));
        CHECK(p.detail.find("connected in 20 ms") != std::string::npos);
        CHECK_EQ(port_summary(ports({PortState::Closed})).word, std::string("Closed"));
        CHECK(port_summary(ports({PortState::Closed})).mark == Mark::Cross);
        CHECK_EQ(port_summary(ports({PortState::Filtered})).word, std::string("No answer"));
        CHECK(port_summary(ports({PortState::Filtered})).tone == Tone::Warn);
        CHECK(port_summary(ports({PortState::Filtered})).mark == Mark::Dash);
        CHECK_EQ(port_summary(ports({PortState::Unreachable})).word, std::string("Unreachable"));
        CHECK(port_summary(ports({PortState::Unreachable})).mark == Mark::Bang);
        // several ports: one word when they agree, else "Mixed" with each port in the detail
        CHECK_EQ(port_summary(ports({PortState::Open, PortState::Open})).word, std::string("Open"));
        p = port_summary(ports({PortState::Filtered, PortState::Open}));
        CHECK_EQ(p.word, std::string("Mixed"));
        CHECK(p.detail.find("80 No answer") != std::string::npos && p.detail.find("81 Open 20 ms") != std::string::npos);
        PortSnapshot failed;
        failed.state = PortSnapshot::State::Failed;
        failed.error = "x does not exist (NXDOMAIN)";
        CHECK_EQ(port_summary(failed).word, std::string("Not checked"));
        CHECK_EQ(port_summary(PortSnapshot()).detail, std::string("Choose a host and a port, then press Run"));
        CHECK_EQ(port_choice_text("Mail", 25), std::string("Mail 25, 465, 587, 993"));
        CHECK_EQ(port_choice_text("", 443), std::string("443 (https)"));
    }

    // saving
    CHECK(save_band(true, "/tmp/x/ping.txt").tone == Tone::Ok);
    CHECK_EQ(save_band(true, "/tmp/x/ping.txt").text, std::string("Saved /tmp/x/ping.txt"));
    CHECK(save_band(false, "Cannot write").tone == Tone::Bad);
}

/* ---------------------------------------------------------------- 0.3: the chosen network on the pages */

namespace {

/* the recorded boards with iw's answer for every wireless interface that has one */
NetState board_iw(const char *name)
{
    const auto src = make_fixture_source(fixture_dir(name));
    NetState st = read_net_state(*src);
    for (const Iface &i : st.ifaces) {
        if (!i.wireless || !i.connected()) continue;
        const std::string text = fixture(name, ("cmd/iw_dev_" + i.name + "_link").c_str());
        if (!text.empty()) apply_iw_link(st, i.name, parse_iw_link(text));
    }
    return st;
}

/* the rows of one interface: those after its heading up to the next heading */
std::vector<RowDef> section_of(const std::vector<RowDef> &rows, const std::string &heading_start)
{
    std::vector<RowDef> out;
    bool in = false;
    for (const RowDef &r : rows) {
        if (r.section) in = r.label.rfind(heading_start, 0) == 0;
        else if (in) out.push_back(r);
    }
    return out;
}

const RowDef *row_in(const std::vector<RowDef> &rows, const char *label)
{
    for (const RowDef &r : rows)
        if (r.label == label) return &r;
    return nullptr;
}

std::string headings(const std::vector<RowDef> &rows)
{
    std::string out;
    for (const RowDef &r : rows)
        if (r.section) out += (out.empty() ? "" : "|") + r.label;
    return out;
}

} // namespace

void test_ui_scope()
{
    // Home: the row's value and the band
    {
        const NetState deck = board("deck", false);
        CHECK_EQ(network_value(resolve_scope("", deck)), std::string("Automatic (wlan0)"));
        CHECK_EQ(network_value(resolve_scope("wlan0", deck)), std::string("wlan0"));
        CHECK_EQ(network_value(resolve_scope("wlan1", deck)), std::string("Automatic (wlan0)")); /* gone: Automatic */
        CHECK_EQ(network_value(resolve_scope("", board("deck-offline", false))), std::string("Automatic"));
        // Automatic: the band is what it was
        CHECK_EQ(home_band(deck, resolve_scope("", deck)).text, home_band(deck).text);
        // the chosen interface and its address
        const NetState two = board("pi-3a-plus-dongle", false);
        Band b = home_band(two, resolve_scope("wlan1", two));
        CHECK_EQ(b.text, std::string("wlan1: 172.20.10.2"));
        CHECK(b.tone == Tone::Ok);
        CHECK_EQ(home_band(two, resolve_scope("", two)).text, std::string("wlan0: 192.168.68.170"));
        // chosen but not connected, and the interface that is gone
        const NetState off = board("pi-3a-plus-dongle-off", false);
        b = home_band(off, resolve_scope("wlan1", off));
        CHECK(b.text == "wlan1: not connected" && b.tone == Tone::Warn);
        const NetState without = board("pi-3a-plus", false);
        b = home_band(without, resolve_scope("wlan1", without));
        CHECK_EQ(b.text, std::string("wlan1 not found: using Automatic (wlan0: 192.168.68.170)"));
        CHECK(b.tone == Tone::Warn);
        const NetState none = board("deck-offline", false);
        CHECK_EQ(home_band(none, resolve_scope("wlan1", none)).text,
                 std::string("wlan1 not found: using Automatic (No network)"));
    }
    // the choice page
    {
        const NetState two = board("pi-3a-plus-dongle", false);
        NetworkChoices c = network_choices(two, resolve_scope("", two));
        CHECK((c.labels == std::vector<std::string>{"Automatic (wlan0)", "wlan0  192.168.68.170", "wlan1  172.20.10.2"}));
        CHECK((c.names == std::vector<std::string>{"", "wlan0", "wlan1"}) && c.current == 0);
        c = network_choices(two, resolve_scope("wlan1", two));
        CHECK(c.current == 2 && c.labels[0] == "Automatic (wlan0)"); /* the one in use is checked */
        // a down interface is listed, selectable, with the words
        const NetState off = board("pi-3a-plus-dongle-off", false);
        c = network_choices(off, resolve_scope("wlan1", off));
        CHECK(c.labels.size() == 3 && c.labels[2] == "wlan1  not connected" && c.names[2] == "wlan1" && c.current == 2);
        // Ethernet: speed beside the address; a cable that is out says so
        const NetState eth = board("eth-wifi", false);
        c = network_choices(eth, resolve_scope("end0", eth));
        CHECK(c.labels[0] == "Automatic (end0)" && c.labels[1] == "end0  192.168.1.20" &&
              c.labels[2] == "wlan0  192.168.1.41");
        CHECK(c.current == 1);
        const NetState cable = board("eth-down", false);
        c = network_choices(cable, resolve_scope("", cable));
        CHECK(c.labels.size() == 3 && c.labels[0] == "Automatic (wlan0)" && c.labels[1] == "wlan0  192.168.1.41" &&
              c.labels[2] == "enx00e04c680a12  no link");
        // the loopback is never listed; the chosen one that is gone: Automatic is checked
        for (const std::string &n : c.names) CHECK(n != "lo");
        const NetState without = board("pi-3a-plus", false);
        c = network_choices(without, resolve_scope("wlan1", without));
        CHECK(c.current == 0 && c.labels.size() == 2);
    }
    // the Interfaces page: marks, and the Wi-Fi rows of every wireless interface
    {
        const NetState two = board_iw("pi-3a-plus-dongle");
        std::vector<RowDef> rows = interface_rows(two, resolve_scope("wlan1", two));
        CHECK_EQ(headings(rows), std::string("wlan0 (default route)|wlan1 (in use)|lo|Network"));
        const std::vector<RowDef> w0 = section_of(rows, "wlan0"), w1 = section_of(rows, "wlan1");
        CHECK(row_in(w0, "Network") && row_in(w0, "Network")->value == "FantaCitron");
        CHECK(row_in(w1, "Network") && row_in(w1, "Network")->value == "iPhone pro OSR");
        CHECK(row_in(w0, "Bit rate") && row_in(w0, "Bit rate")->value == "72.2 Mbit/s");
        CHECK(row_in(w1, "Bit rate") && row_in(w1, "Bit rate")->value == "130 Mbit/s");
        CHECK(row_in(w1, "Signal") && row_in(w1, "Signal")->value == "-52 dBm, Good"); /* iw's, over /proc/net/wireless */
        CHECK(row_in(w1, "IPv4") && row_in(w1, "IPv4")->value == "172.20.10.2/28");
        // Automatic: only the default route is marked
        rows = interface_rows(two, resolve_scope("", two));
        CHECK_EQ(headings(rows), std::string("wlan0 (default route)|wlan1|lo|Network"));
        // the chosen one is also the default route
        rows = interface_rows(two, resolve_scope("wlan0", two));
        CHECK_EQ(headings(rows), std::string("wlan0 (in use, default route)|wlan1|lo|Network"));
        // one interface only: no marks to tell apart; the old overload is Automatic
        CHECK_EQ(headings(interface_rows(board("deck", true))), std::string("wlan0|lo|Network"));
        // a Wi-Fi interface that is not connected: "Not connected", no Network, Signal or Bit rate rows
        const NetState off = board_iw("pi-3a-plus-dongle-off");
        rows = interface_rows(off, resolve_scope("wlan1", off));
        const std::vector<RowDef> d1 = section_of(rows, "wlan1");
        CHECK(row_in(d1, "State") && row_in(d1, "State")->value == "Not connected");
        CHECK(!row_in(d1, "Network") && !row_in(d1, "Signal") && !row_in(d1, "Bit rate"));
        CHECK(row_in(section_of(rows, "wlan0"), "Network")); /* the other one still has its own */
        // a stale answer for an interface that is no longer connected is not shown
        NetState stale = board("pi-3a-plus-dongle-off", false);
        apply_iw_link(stale, "wlan1", parse_iw_link(fixture("pi-3a-plus-dongle", "cmd/iw_dev_wlan1_link")));
        rows = interface_rows(stale, resolve_scope("", stale));
        CHECK(!row_in(section_of(rows, "wlan1"), "Network") && !row_in(section_of(rows, "wlan1"), "Bit rate"));
    }
    {
        // wired and Wi-Fi, wired chosen; the cable out
        const NetState eth = board_iw("eth-wifi");
        std::vector<RowDef> rows = interface_rows(eth, resolve_scope("end0", eth));
        CHECK_EQ(headings(rows), std::string("end0 (in use, default route)|wlan0|lo|Network"));
        const std::vector<RowDef> e = section_of(rows, "end0");
        CHECK(row_in(e, "State") && row_in(e, "State")->value == "Connected");
        CHECK(row_in(e, "Speed") && row_in(e, "Speed")->value == "1 Gbit/s" && !row_in(e, "Network"));
        const NetState cable = board_iw("eth-down");
        rows = interface_rows(cable, resolve_scope("enx00e04c680a12", cable));
        CHECK_EQ(headings(rows), std::string("wlan0 (default route)|enx00e04c680a12 (in use)|lo|Network"));
        const std::vector<RowDef> u = section_of(rows, "enx00e04c680a12");
        CHECK(row_in(u, "State") && row_in(u, "State")->value == "No link" && row_in(u, "State")->tone == Tone::Warn);
        CHECK(row_in(u, "Speed") && row_in(u, "Speed")->value == "unknown");
        // three interfaces, two of them Wi-Fi
        const NetState all = board_iw("pi-eth-two-wifi");
        rows = interface_rows(all, resolve_scope("wlan1", all));
        CHECK_EQ(headings(rows), std::string("end0 (default route)|wlan0|wlan1 (in use)|lo|Network"));
        CHECK(row_in(section_of(rows, "wlan1"), "Network")->value == "iPhone pro OSR");
        CHECK(row_in(section_of(rows, "wlan0"), "Network")->value == "FantaCitron");
    }
    // the DNS rows name whose servers they are
    CHECK_EQ(resolver_label("172.20.10.1", "172.20.10.1", "wlan1's DNS"), std::string("wlan1's DNS 172.20.10.1"));
    CHECK_EQ(resolver_label("172.20.10.1", "172.20.10.1"), std::string("System 172.20.10.1"));
    CHECK_EQ(resolver_label("1.1.1.1", "1.1.1.1", "wlan1's DNS"), std::string("Cloudflare 1.1.1.1")); /* presets keep their name */
}
