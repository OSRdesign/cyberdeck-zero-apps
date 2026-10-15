/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.2: the scripted answers of the demonstration backend for the new tools (see backend.hpp: NET_TOOLBOX_DEMO
 * trace=, http=, pubip=, whois=). No network, no thread: the same snapshots the core's sessions publish, built by hand
 * (and through the core's own derive and parse functions), so the scenes and the board's screenshots are the same every
 * time. The clock of a demonstration stands at 2026-10-15 12:00 UTC.
 */
#pragma once

#include "backend.hpp"

#include <ctime>
#include <string>

namespace nt {
namespace ui {

std::time_t demo_now();

/* a trace as it runs (Running, round 14); stop() turns it into Cancelled */
TraceSnapshot demo_trace(const std::string &scenario, const TraceConfig &cfg);
HttpCheckSnapshot demo_http(const std::string &scenario, const HttpCheckRequest &req);
PublicIpSnapshot demo_pubip(const std::string &scenario);
WhoisSnapshot demo_whois(const std::string &scenario, const WhoisRequest &req);

} // namespace ui
} // namespace nt
