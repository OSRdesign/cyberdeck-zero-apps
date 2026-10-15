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

#include "diagnosis.hpp"
#include "dns_client.hpp"
#include "interfaces.hpp"
#include "ping.hpp"
#include "portcheck.hpp"

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

} // namespace nt
