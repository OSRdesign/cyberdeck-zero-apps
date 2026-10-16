/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the OUI vendor lookup the Wi-Fi pages hand to the scan (the table of Local devices, loaded once, lazily, on
 * the first lookup). Apart from the pages because core/oui.hpp and core/wifiscan.hpp both name a VendorLookup and cannot be
 * included together.
 */
#pragma once

#include <string>

namespace nt {
namespace ui {

/* "TP-Link" for "b0:4e:26:00:00:01", "" when the company is not in the table, the file is missing or the address is a
 * locally administered (random) one. Safe to call from the scan's worker thread. */
std::string oui_vendor_of(const std::string &mac);

} // namespace ui
} // namespace nt
