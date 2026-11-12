/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound server: the text report of what the server lists, for the command line tools: `cassette-probe --server`
 * (packaged) and `pulse-capture-test` (the board CLI of T1) print the same summary lines with these functions.
 * Listen-only: lists, never opens a stream. LVGL-free.
 */
#pragma once

#include "pulse_probe.hpp"

#include <string>

namespace cassette {

/* The "summary:" block: one line per source / sink (name, bt=yes|no, mac, kind, rate, monitor, default) and one per
 * Bluetooth card (music available / offered but unavailable / not offered, call offered or not, the active profile and
 * whether it is music or call). mac_filter (upper or lower case, "" = all): only the devices and cards of that MAC. */
std::string pulse_probe_summary(const PulseProbe &probe, const std::string &mac_filter = std::string());

/* cassette-probe --server [--card bt:<MAC>]: connects (never starts a server), prints the server line, the defaults and the
 * summary into out, disconnects. Returns 0 when listed (and, with a filter, the device was there), 1 when the library or
 * the server is missing, the listing failed, or the filtered device is not on the server. */
int pulse_server_report(PulseApi &api, const std::string &mac_filter, std::string &out);

}  // namespace cassette
