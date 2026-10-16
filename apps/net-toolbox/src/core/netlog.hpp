/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: results saved as text (no LVGL; tests/test_core.cpp).
 *
 *   save_log()    writes ~/net-logs/<tool>-<YYYYmmdd-HHMMSS>.txt (the folder is made when missing; NET_TOOLBOX_LOG_DIR
 *                 names another one; a second save in the same second gets "-2", "-3" ...), through a temporary file
 *                 and a rename. Returns the path, or the reason in plain words.
 *   format_*()    the text of each tool's result: a heading line (app, tool, local time), then the facts, one per line.
 */
#pragma once

#include "ble.hpp"
#include "devices.hpp"
#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "httpcheck.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"
#include "publicip.hpp"
#include "speedtest.hpp"
#include "traceroute.hpp"
#include "whois.hpp"
#include "wol.hpp"

#include <ctime>
#include <string>

namespace nt {

struct SaveResult {
    bool ok = false;
    std::string path;
    std::string error;
};

/* $NET_TOOLBOX_LOG_DIR, else $HOME/net-logs ("" when neither is set) */
std::string default_log_dir();
/* "ping-20261015-140322.txt" for that local time; the tool name keeps [a-z0-9-] only */
std::string log_file_name(const std::string &tool, std::time_t when);
SaveResult save_log(const std::string &dir, const std::string &tool, const std::string &text, std::time_t when);

/* "Net Toolbox 0.1.0 - Ping - 2026-10-15 14:03:22" */
std::string log_heading(const std::string &version, const std::string &tool_title, std::time_t when);

std::string format_interfaces(const NetState &st);
std::string format_ping(const PingSnapshot &s);
std::string format_dns(const DnsSnapshot &s);
std::string format_ports(const PortSnapshot &s);
std::string format_diagnosis(const DiagReport &r);
/* 0.2: the hops of a summary (up to the destination, trailing silent hops dropped) */
std::string format_trace(const TraceSnapshot &s);
std::string format_httpcheck(const HttpCheckResult &r);
/* interface_ip: the board's address on the default route ("" when unknown), for the NAT line */
std::string format_publicip(const PublicIpResult &r, const std::string &interface_ip = "");
std::string format_whois(const WhoisResult &r);
/* 0.3 */
std::string format_speedtest(const SpeedSnapshot &s);
std::string format_wol(const WolSnapshot &s);
std::string format_ble(const BleSnapshot &s);
/* 0.4 */
std::string format_devices(const DevicesSnapshot &s);
std::string format_portscan(const PortScanSnapshot &s);

} // namespace nt
