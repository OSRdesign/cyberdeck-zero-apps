/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the board's SoC class (brief 4.1 rule 4: table data per SoC class, never board names in logic). From the device
 * tree's compatible list (/proc/device-tree/compatible, else /sys/firmware/devicetree/base/compatible: NUL-separated
 * strings); a PC has none and is Other. LVGL-free.
 *
 *   Bcm2712  Pi 5, CM5 (RPI52, RPI53, pi5b, the HackBerry)
 *   Bcm2711  Pi 4B, CM4 (the deck)
 *   Bcm2837  Pi 3A+ (BCM2837B0) and Zero 2 W (RP3A0 = BCM2710A1): both list "brcm,bcm2837"
 *   Other    everything else (the amd64 tablet, unknown boards)
 */
#pragma once

#include <string>

namespace vhs {

enum class SocClass { Bcm2712, Bcm2711, Bcm2837, Other };

const char *soc_name(SocClass s); /* "bcm2712", ..., "other" */

/* The class of a compatible list (NUL- or newline-separated strings). */
SocClass soc_from_compatible(const std::string &compatible);

/* The running board's (root "" = the real system; a folder = a fixture tree holding proc/device-tree/compatible). */
SocClass detect_soc(const std::string &root = "");

} // namespace vhs
