/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: MAC vendor lookup from the IEEE OUI registry for the Local devices tool (no LVGL, no network;
 * tests/test_oui.cpp).
 *
 * The table ships with the app as `net-toolbox-oui.tsv` under root/usr/share/APPLaunch/share/: one line per prefix,
 * "AABBCC\t<vendor>" (the first three MAC bytes in upper-case hex, a tab, the company name shortened for the small
 * screen). tools/make_oui.py rebuilds it from https://standards-oui.ieee.org/oui/oui.csv (credit in README/copyright).
 *
 * A MAC whose first byte has the locally-administered bit (0x02) set is a private / random address (phones and watches
 * use them for privacy): no vendor is guessed for it; lookup_vendor() returns VendorLookup{private_address = true}.
 * The table is loaded once, at most kMaxEntries lines, and left in memory (about 40 000 prefixes, ~1 MB of strings).
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace nt {

constexpr std::size_t kOuiMaxEntries = 70000;
/* the shipped data file, under /usr/share/APPLaunch/share/ (NET_TOOLBOX_OUI overrides the path, for the tests) */
constexpr const char *kOuiFileName = "net-toolbox-oui.tsv";

/* AABBCC (24-bit OUI, host order) -> vendor; one line "AABBCC\tVendor". Bad lines skipped; bounded at kOuiMaxEntries. */
std::unordered_map<std::uint32_t, std::string> parse_oui_table(const std::string &text);

/* The 24-bit OUI of a MAC text ("aa:bb:cc:...", "aa-bb-cc-...", "aabbcc..."); false for anything that is not a MAC. */
bool mac_oui(const std::string &mac, std::uint32_t &oui);
/* The locally-administered bit (0x02 of the first byte): a random / private address. false for a bad MAC. */
bool mac_is_locally_administered(const std::string &mac);

struct VendorLookup {
    std::string vendor;          /* "" when unknown */
    bool private_address = false; /* a locally-administered (random) MAC: no vendor is guessed */
};

/* Looks `mac` up in `table`. A locally-administered MAC gives {private_address = true} and no vendor. */
VendorLookup lookup_vendor(const std::unordered_map<std::uint32_t, std::string> &table, const std::string &mac);

/* The OUI table, loaded once from kOuiFileName (next to the binary's share dir, or NET_TOOLBOX_OUI). Empty when the
 * file is missing: lookups then only tell private from registered. */
const std::unordered_map<std::uint32_t, std::string> &oui_table();
/* Where oui_table() reads from: $NET_TOOLBOX_OUI, else /usr/share/APPLaunch/share/net-toolbox-oui.tsv */
std::string oui_file_path();

} // namespace nt
