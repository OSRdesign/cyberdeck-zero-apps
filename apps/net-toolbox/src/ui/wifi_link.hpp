/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox: the Wi-Fi network name and bit rate from `/usr/sbin/iw dev <if> link`, run through the shared
 * async_run (LVGL thread only; core/interfaces.hpp has the command line and the parser). Optional: without iw, or when
 * it fails or takes longer than kIwTimeoutMs, `done` gets a link with connected = false and ran = false, and the
 * interfaces keep the signal level of /proc/net/wireless.
 */
#pragma once

#include "interfaces.hpp"
#include "scope.hpp"

#include "async_run.hpp"

#include <functional>
#include <string>

namespace nt {

/* `done` runs once, on the LVGL thread (also when the command could not start). Returns the job (0: not started). */
cdk::async_run::JobId request_wifi_link(const std::string &iface,
                                        std::function<void(const IwLink &link, bool ran)> done);

/* 0.3: the DNS servers of an interface, `nmcli -g IP4.DNS,IP6.DNS device show <iface>` (scope.hpp has the command line and
 * the parser), the same way. `done(text, ran)` runs once on the LVGL thread; ran = false when nmcli is not installed, took
 * longer than kNmcliTimeoutMs or failed (the caller then falls back to the interface's gateway). */
cdk::async_run::JobId request_nmcli_dns(const std::string &iface, std::function<void(const std::string &text, bool ran)> done);

} // namespace nt
