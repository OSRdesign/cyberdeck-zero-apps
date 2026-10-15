/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: what the pages say (no LVGL, no network: tests/test_ui_model.cpp). Every text, tone and number a page
 * shows is made here from the core's snapshots, so the pages only place it:
 *
 *   Home band, Interfaces rows (Wi-Fi signal words), the Diagnosis ladder's words and message, the Ping headline,
 *   statistics line, packet cells and graph scale, the DNS status line and result rows, the Port check summary.
 *
 * A tone is a status (Ok green, Warn amber, Bad red); the pages never show it without the words or a shape.
 */
#pragma once

#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nt {
namespace ui {

enum class Tone { Normal, Ok, Warn, Bad };

/* the mark of a state (a shape, not only a colour) */
enum class Mark { None, Check, Cross, Dash, Bang };

/* one row of a list page: a heading (section) or a label with a value */
struct RowDef {
    bool section = false;
    std::string label;
    std::string value;
    Tone tone = Tone::Normal;
    int id = 0;
};

/* ---------------------------------------------------------------- Home, Interfaces */

struct Band {
    std::string text;
    Tone tone = Tone::Normal;
};

/* "wlan0: 192.168.68.121" (Ok) or "No network" (Warn) */
Band home_band(const NetState &st);

/* Wi-Fi signal: Excellent from -50 dBm, Good from -60, Fair from -70, else Weak */
const char *signal_word(double dbm);
Tone signal_tone(double dbm);
/* "-52 dBm, Excellent" */
std::string signal_text(double dbm);

/* A heading per interface (State, IPv4, IPv6, MAC, Speed; Wi-Fi: Network, Signal, Bit rate), then the heading "Network"
 * with the gateway, the DNS servers in resolv.conf order (the first marked) and the search domains. */
std::vector<RowDef> interface_rows(const NetState &st);

/* ---------------------------------------------------------------- Diagnosis */

/* the state in words and the detail of a rung ("3 ms", "waiting", "checking...", "skipped") */
std::string step_text(const DiagStep &step);
Tone step_tone(StepState state);

struct DiagMessage {
    std::string head; /* "Everything works", "DNS failed", "Checking your connection..." */
    std::string body; /* the core's explanation, or its notes one per line */
    Tone tone = Tone::Normal;
};
DiagMessage diag_message(const DiagReport &report);

/* ---------------------------------------------------------------- Ping */

/* 10, 15, 20, 30, 50, 75, 100, 150 ... the first of these over max_ms (with 10 % of room), at least 10 */
double nice_top_ms(double max_ms);
/* the largest round trip of the packets that were answered, 0 when none */
double max_rtt_ms(const std::vector<PacketResult> &packets);

/* the slot of packet i of n in the graph and the strip: the newest at the right edge (kRingSize slots) */
int ring_slot(std::size_t i, std::size_t n);

struct Cell {
    PacketState state = PacketState::Pending;
    int height_pct = 0; /* of the strip: ok 45, slow 70, lost 100, pending 25: not only the colour tells them apart */
};
Cell packet_cell(PacketState state);
const char *packet_word(PacketState state); /* "ok", "slow", "lost", "waiting" */

/* "now 12 ms", "now slow 240 ms", "lost" */
Band ping_head(const PingSnapshot &s);
/* "scale 0-50 ms, last 120 packets" */
std::string ping_scale(double top_ms, std::size_t packets);
/* "sent 42  lost 3 (7%)  min 9.1 ms  avg 12 ms  max 31 ms  jitter 2.0 ms" */
std::string ping_stats(const PingStats &st);
/* the page's status line */
Band ping_band(const PingSnapshot &s);

/* ---------------------------------------------------------------- DNS lookup */

/* "System 192.168.1.1" for the first system server, "Cloudflare 1.1.1.1" for a preset, else the address */
std::string resolver_label(const std::string &server, const std::string &system_server);
Band dns_status(const DnsSnapshot &s);
/* the result list: a heading, then a row per answer (data, with "TYPE ttl" at the right) or per resolver (compare) */
std::vector<RowDef> dns_rows(const DnsSnapshot &s, const std::string &system_server);

/* ---------------------------------------------------------------- Port check */

struct PortSummary {
    std::string word;   /* Open, Closed, No answer, Unreachable, Mixed, Checking... */
    std::string detail; /* what it means, with the connect time */
    std::string host;   /* "example.com (93.184.215.14)" */
    Tone tone = Tone::Normal;
    Mark mark = Mark::None;
};
const char *port_word(PortState state);
PortSummary port_summary(const PortSnapshot &s);

/* "Web 80, 443" for a preset, "443 (https)" for one port */
std::string port_choice_text(const std::string &preset, int port);

/* ---------------------------------------------------------------- the status line after Save log */

Band save_band(bool ok, const std::string &path_or_error);

} // namespace ui
} // namespace nt
