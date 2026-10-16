/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: MAC vendor lookup (see oui.hpp).
 */
#include "oui.hpp"

#include "text.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>

namespace nt {

namespace {

/* one hex nibble, or -1 */
int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the first n hex bytes of a MAC text into bytes[]; separators ':' and '-' allowed but consistent; true on exactly
 * `n` bytes read and the rest of the text being a valid continuation (we only parse the prefix we need) */
bool read_hex_bytes(const std::string &mac, int n, std::uint8_t *bytes)
{
    int got = 0;
    std::size_t i = 0;
    while (got < n && i < mac.size()) {
        const int hi = hex_nibble(mac[i]);
        if (hi < 0) return false;
        if (i + 1 >= mac.size()) return false;
        const int lo = hex_nibble(mac[i + 1]);
        if (lo < 0) return false;
        bytes[got++] = static_cast<std::uint8_t>((hi << 4) | lo);
        i += 2;
        if (got < n && i < mac.size() && (mac[i] == ':' || mac[i] == '-')) ++i; /* skip one separator */
    }
    return got == n;
}

} // namespace

std::unordered_map<std::uint32_t, std::string> parse_oui_table(const std::string &text)
{
    std::unordered_map<std::uint32_t, std::string> table;
    table.reserve(45000);
    for (const std::string &line : split_lines(text)) {
        if (line.size() < 8 || line[6] != '\t') continue;
        std::uint8_t b[3];
        if (!read_hex_bytes(line.substr(0, 6), 3, b)) continue;
        const std::uint32_t oui = (static_cast<std::uint32_t>(b[0]) << 16) | (static_cast<std::uint32_t>(b[1]) << 8) |
                                  static_cast<std::uint32_t>(b[2]);
        std::string vendor = trim(line.substr(7));
        if (vendor.empty()) continue;
        if (vendor.size() > 40) vendor.resize(40);
        table.emplace(oui, std::move(vendor));
        if (table.size() >= kOuiMaxEntries) break;
    }
    return table;
}

bool mac_oui(const std::string &mac, std::uint32_t &oui)
{
    std::uint8_t b[3];
    if (!read_hex_bytes(mac, 3, b)) return false;
    oui = (static_cast<std::uint32_t>(b[0]) << 16) | (static_cast<std::uint32_t>(b[1]) << 8) |
          static_cast<std::uint32_t>(b[2]);
    return true;
}

bool mac_is_locally_administered(const std::string &mac)
{
    std::uint8_t b[1];
    if (!read_hex_bytes(mac, 1, b)) return false;
    return (b[0] & 0x02) != 0;
}

VendorLookup lookup_vendor(const std::unordered_map<std::uint32_t, std::string> &table, const std::string &mac)
{
    VendorLookup r;
    std::uint8_t first[1];
    if (!read_hex_bytes(mac, 1, first)) return r; /* not a MAC: nothing */
    if (first[0] & 0x02) {
        r.private_address = true; /* locally administered: a random address, no vendor */
        return r;
    }
    std::uint32_t oui = 0;
    if (!mac_oui(mac, oui)) return r;
    const auto it = table.find(oui);
    if (it != table.end()) r.vendor = it->second;
    return r;
}

std::string oui_file_path()
{
    const char *over = std::getenv("NET_TOOLBOX_OUI");
    if (over && over[0]) return over;
    return std::string("/usr/share/APPLaunch/share/") + kOuiFileName;
}

const std::unordered_map<std::uint32_t, std::string> &oui_table()
{
    static std::unordered_map<std::uint32_t, std::string> table;
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string path = oui_file_path();
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        std::string text;
        char buf[65536];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) text.append(buf, static_cast<std::size_t>(n));
        close(fd);
        table = parse_oui_table(text);
    });
    return table;
}

} // namespace nt
