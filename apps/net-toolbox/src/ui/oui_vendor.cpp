/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the OUI vendor lookup of the Wi-Fi pages (see oui_vendor.hpp).
 */
#include "oui_vendor.hpp"

#include "oui.hpp"

namespace nt {
namespace ui {

std::string oui_vendor_of(const std::string &mac)
{
    return lookup_vendor(oui_table(), mac).vendor;
}

} // namespace ui
} // namespace nt
