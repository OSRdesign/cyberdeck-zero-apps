/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: what the pages say (no LVGL, no network: tests/test_ui_model.cpp). Every text, tone and number a page
 * shows is made here from the core's snapshots, so the pages only place it:
 *
 *   Home band, Interfaces rows (Wi-Fi signal words), the Diagnosis ladder's words and message, the Ping headline,
 *   statistics line, packet cells and graph scale, the DNS status line and result rows, the Port check summary.
 *   0.2: the traceroute chain (hop looks, names, loss tags, bar lengths, the status line and the verdict, the details of
 *   a hop), the HTTP check (status badge, waterfall segments, certificate badge and rows), the public address rows and
 *   the whois summary rows, with the "days left" wording they share.
 *
 * A tone is a status (Ok green, Warn amber, Bad red); the pages never show it without the words or a shape.
 */
#pragma once

#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"
#include "httpcheck.hpp"
#include "publicip.hpp"
#include "scope.hpp"
#include "traceroute.hpp"
#include "whois.hpp"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace nt {
namespace ui {

enum class Tone { Normal, Ok, Warn, Bad };

/* the mark of a state (a shape, not only a colour) */
enum class Mark { None, Check, Cross, Dash, Bang, Arrow };

/* What a row does when it is activated, shown by its look (the launcher's row-affordance rule, cp0_ui_list.h):
 *   Info           a read-out: grey value, no marker (an explicit Ok / Warn / Bad tone keeps its colour)
 *   Open           opens another page: a chevron at the right edge
 *   Edit           changes a value (in place or through a picker): the value in its tone, then the chevron
 *   Action         does something right there (start, stop, save, send): a white dot in the chevron's slot
 *   ActionRunning  that action is in progress: the dot in the accent blue
 *   Unspecified    drawn as before affordances existed (paragraphs, blocks that are only scrolled to) */
enum class Aff { Unspecified, Info, Open, Edit, Action, ActionRunning };

/* the dot of an action row: blue while the action runs (a Stop row), white while it is available */
inline Aff action_aff(bool running) { return running ? Aff::ActionRunning : Aff::Action; }

/* one row of a list page: a heading (section) or a label with a value; a view-model row is a read-out (Info) unless the
 * page says otherwise */
struct RowDef {
    bool section = false;
    std::string label;
    std::string value;
    Tone tone = Tone::Normal;
    int id = 0;
    Aff aff = Aff::Info;
};

/* ---------------------------------------------------------------- Home, Interfaces */

struct Band {
    std::string text;
    Tone tone = Tone::Normal;
};

/* "wlan0: 192.168.68.121" (Ok) or "No network" (Warn) */
Band home_band(const NetState &st);
/* 0.3, with the chosen network: the effective interface and its address ("wlan1: 172.20.10.2"; "wlan1: not connected" and
 * "wlan1: no address yet" in amber). A chosen interface that is gone: "wlan1 not found: using Automatic (wlan0:
 * 192.168.68.170)" in amber. Automatic = the one above. */
Band home_band(const NetState &st, const NetScope &scope);

/* The Home row "Network": "Automatic (wlan0)", "Automatic" when there is no main interface, "wlan1" */
std::string network_value(const NetScope &scope);
/* The choice page: [0] "Automatic (wlan0)", then every interface but the loopback as "wlan1  172.20.10.2",
 * "wlan1  not connected" (Wi-Fi), "enx00e04c680a12  no link" (a cable that is out): a down one is listed, selectable;
 * the speed is on the Interfaces page. names[k] is the interface
 * of labels[k], "" for Automatic. current: the row of the interface in use (0 = Automatic). */
struct NetworkChoices {
    std::vector<std::string> labels;
    std::vector<std::string> names;
    int current = 0;
};
NetworkChoices network_choices(const NetState &st, const NetScope &scope);

/* Wi-Fi signal: Excellent from -50 dBm, Good from -60, Fair from -70, else Weak */
const char *signal_word(double dbm);
Tone signal_tone(double dbm);
/* "-52 dBm, Excellent" */
std::string signal_text(double dbm);

/* A heading per interface (State, IPv4, IPv6, MAC, Speed; Wi-Fi: Network, Signal, Bit rate), then the heading "Network"
 * with the gateway, the DNS servers in resolv.conf order (the first marked) and the search domains. */
std::vector<RowDef> interface_rows(const NetState &st);
/* 0.3: the headings mark the chosen interface "wlan1 (in use)" and the system's main one "wlan0 (default route)" (both:
 * "wlan0 (in use, default route)"); a Wi-Fi interface that is not connected says so and has no Network, Signal or Bit rate
 * row. Automatic marks only the default route, and only when there is more than one interface to tell apart. */
std::vector<RowDef> interface_rows(const NetState &st, const NetScope &scope);

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
std::string resolver_label(const std::string &server, const std::string &system_server,
                           const std::string &system_name = "System");
Band dns_status(const DnsSnapshot &s);
/* the result list: a heading, then a row per answer (data, with "TYPE ttl" at the right) or per resolver (compare) */
std::vector<RowDef> dns_rows(const DnsSnapshot &s, const std::string &system_server,
                             const std::string &system_name = "System");

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

/* ---------------------------------------------------------------- shared: days left, sizes */

/* "expires in 83 days", "expires in 1 day", "expires today", "expired 1 day ago", "expired 12 days ago" */
std::string days_words(int days);
/* green from 30 days, amber under 30, red under 7 and once expired */
Tone days_tone(int days);

/* "0 bytes", "812 bytes", "1.2 kB", "3.4 MB" */
std::string size_text(std::uint64_t bytes);

/* ---------------------------------------------------------------- Traceroute */

/* What a hop's node looks like (a shape and a colour, never the colour alone):
 *   Pending      the first probes are still out: a thin hollow ring
 *   Ok           a filled green circle
 *   Slow         an amber circle with a notch: loss of 1 to 20 %, or an average over 150 ms
 *   Lossy        a red circle with an X: more than 20 % loss that later hops share (real loss)
 *   Silent       a hollow dotted circle: nothing ever answered (normal: many routers do not)
 *   RateLimited  a grey ring: loss at this hop only, later hops are clean (probably the router limiting its answers) */
enum class HopLook { Pending, Ok, Slow, Lossy, Silent, RateLimited };

constexpr double kHopSlowAvgMs = 150.0;
constexpr double kHopLossBadPct = 20.0;

HopLook hop_look(const TraceHop &h);
/* "ok", "slow or some loss", "real loss", "no reply", "probably rate limiting", "waiting" */
const char *hop_look_word(HopLook look);

struct HopTexts {
    HopLook look = HopLook::Pending;
    bool destination = false;     /* double ring, and the word "reached" */
    std::string number;           /* "7" */
    std::string name;             /* the reverse name when known, else the address, else "" */
    std::string ip;               /* the address beside a name ("" when the name is the address) */
    std::string avg;              /* "43 ms", "" when nothing answered */
    std::string loss;             /* "30 %" when something was lost and the hop is not silent */
    std::string bar_word;         /* in the bar's place: "no reply", "waiting" */
    std::string note;             /* "reached", "probably rate limiting", a router's "host unreachable" */
    Tone note_tone = Tone::Normal;
    Tone loss_tone = Tone::Normal;
    bool has_bar = false;
    double avg_ms = 0, best_ms = 0, worst_ms = 0;
};
HopTexts hop_texts(const TraceHop &h);

/* "30 %" (at least "1 %" for any loss); "" for no loss, a silent or a waiting hop */
std::string loss_tag(const TraceHop &h);

/* the shared scale: the largest average of the first `shown` hops, at least 1 ms */
double chain_scale_ms(const std::vector<TraceHop> &hops, std::size_t shown);
/* a bar of `ms` on `scale_ms` over full_px: never under stub_px for a value above 0, never over full_px */
int bar_px(double ms, double scale_ms, int full_px, int stub_px);

/* "to example.com 93.184.215.14, round 3, 9 hops" (with_address false: "to example.com, round 3, 9 hops") */
std::string trace_progress(const TraceSnapshot &s, bool with_address = true);
/* "Reached in 9 hops, 43 ms" / "Did not reach the destination: no answer after hop 6" / "... unreachable at hop 5" */
std::string trace_verdict(const TraceSnapshot &s);
/* the page's status line: Press Start, Finding, the progress, the verdict (green reached, amber not reached, red
 * unreachable or failed). idle_target: what Start will trace, for the idle line. */
Band trace_band(const TraceSnapshot &s, const std::string &idle_target, bool with_address = true);

/* The lines of a hop's details (the row opened in place). */
struct HopDetail {
    std::string addresses; /* "62.1.2.3 core-b.example, 30 answers; 62.1.2.9, 2 answers" */
    std::string timing;    /* "last 42 ms  avg 43 ms  best 38 ms  worst 52 ms  jitter 2.1 ms" */
    std::string counts;    /* "sent 30  received 29  lost 1 (3 %)" */
    std::string note;      /* the plain-words reading of the node */
};
HopDetail hop_detail(const TraceHop &h);

/* ---------------------------------------------------------------- HTTP check */

/* The big badge: "200 OK" (green check), "301 moved" (amber arrow), "404 Not Found" / "500 ..." (red cross), an error
 * (red cross, the problem's name, the plain words in detail), Checking..., Stopped. host: the address asked. */
PortSummary http_badge(const HttpCheckSnapshot &s);
Band http_band(const HttpCheckSnapshot &s);

enum class WaterPart { Dns, Connect, Tls, Wait, Transfer };
struct WaterSeg {
    WaterPart part = WaterPart::Dns;
    const char *name = "";    /* "DNS", "Connect", "TLS", "Waiting", "Transfer" */
    double ms = 0;
    int x = 0, w = 0;         /* in the bar; w is 0 only for a segment of 0 ms */
};
/* The segments that happened (DNS always; Connect when connected; TLS for https; Waiting and Transfer once the answer
 * began) on one scale over width_px; a segment above 0 ms is at least min_px wide; the widths add up to width_px. */
std::vector<WaterSeg> waterfall(const HttpTimings &t, int width_px, int min_px = 2);
/* "Total 183 ms" */
std::string waterfall_total(const HttpTimings &t);
/* "DNS 12 ms" */
std::string water_label(const WaterSeg &seg);

struct CertBadge {
    std::string word;      /* "expires in 83 days", "expired 12 days ago", "no certificate", "not read" */
    std::string detail;    /* the subject, or why there is none */
    Tone tone = Tone::Normal;
    Mark mark = Mark::None;
    bool has_days = false;
    int days = 0;
};
CertBadge cert_badge(const HttpCheckResult &r);

/* the rows around the blocks: "Final URL" (when it differs); the certificate's Subject, Issuer, Valid until; Remote IP,
 * HTTP version, Size, Redirects. Rows with nothing to say are left out. */
std::vector<RowDef> http_final_rows(const HttpCheckResult &r);
std::vector<RowDef> http_cert_rows(const HttpCheckResult &r);
std::vector<RowDef> http_info_rows(const HttpCheckResult &r);

/* ---------------------------------------------------------------- Public IP */

/* "Switzerland (CH)" for the codes in the table, else the code as given */
std::string country_text(const std::string &code);
/* The note for carrier-grade NAT ("" for anything else). */
std::string nat_note(const std::string &interface_ip, const PublicIpResult &r);
/* Public IPv4, Public IPv6 ("none" is normal), Country, Cloudflare data centre, Local address */
std::vector<RowDef> publicip_rows(const PublicIpSnapshot &s, const std::string &interface_ip);
Band publicip_band(const PublicIpSnapshot &s);
/* the address for "Whois of this address": the IPv4, else the IPv6, else "" */
std::string publicip_address(const PublicIpSnapshot &s);

/* ---------------------------------------------------------------- Whois */

/* "2026-08-13", "13-aug-2026", "2026.08.13", "2026/08/13", an ISO time: the day as UTC seconds (noon) and as YYYY-MM-DD */
bool whois_day(const std::string &text, std::int64_t &epoch, std::string *iso = nullptr);
/* the summary rows: domain (Registrar, Created, Updated, Expires with the days left in words, Status, Name server...) or
 * address (Net name, Organisation, Country, Range, AS number, Abuse contact); an empty field is not a row */
std::vector<RowDef> whois_rows(const WhoisResult &r, std::time_t now);
/* the raw answers as lines: "-- whois.arin.net --" before each answer, then its lines; at most max_lines (the rest is
 * counted in *more) */
std::vector<std::string> whois_raw(const WhoisResult &r, std::size_t max_lines, std::size_t *more = nullptr);
Band whois_band(const WhoisSnapshot &s);

/* ---------------------------------------------------------------- the status line after Save log */

Band save_band(bool ok, const std::string &path_or_error);

} // namespace ui
} // namespace nt
