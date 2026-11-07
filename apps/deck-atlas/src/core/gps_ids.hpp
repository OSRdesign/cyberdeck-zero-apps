/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: which USB serial devices Port = Auto may open. Auto opens only a device whose USB identity (idVendor and
 * idProduct, read from sysfs next to the tty) is on the short list of GPS receivers in gps_ids.cpp. Every other serial
 * port (a CH340 radar, an ESP32 or LoRa board, a CP210x or FTDI adapter...) is never opened by Auto, because opening a
 * port raises DTR and some boards reset on it; those ports are listed in Settings > GPS receiver > Port, and a port the
 * user chose there is opened as before. Pure: no I/O.
 */
#pragma once

#include <cstdint>

namespace atlas {

/* True when vid:pid is a known GPS receiver. */
bool gps_known_receiver(std::uint16_t vid, std::uint16_t pid);
/* The product the id stands for ("u-blox GNSS receiver"), "" when the id is not on the list. */
const char *gps_receiver_name(std::uint16_t vid, std::uint16_t pid);

} // namespace atlas
