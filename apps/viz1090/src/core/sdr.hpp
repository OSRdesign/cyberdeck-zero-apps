/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: is an RTL-SDR dongle plugged in? The USB ids (vendor:product) of the RTL2832U receivers that librtlsdr
 * knows (its known_devices table), matched against /sys/bus/usb/devices/<dev>/idVendor and idProduct. Reads sysfs only
 * (a few small files, fast enough for the 1 s re-check on the LVGL thread). Pure C++, no LVGL: unit-tested.
 */
#pragma once

#include <cstddef>
#include <string>

namespace viz {

struct UsbId {
    const char *vid;
    const char *pid;
};

/* the table and its length */
const UsbId *sdr_ids(std::size_t *count);
/* vid / pid as sysfs writes them (4 hex digits, lower case; upper case accepted) */
bool is_sdr_id(const std::string &vid, const std::string &pid);

/* $VIZ_USB_DIR (tests), else /sys/bus/usb/devices */
std::string usb_devices_dir();
/* The first dongle under `dir`: its name ("Realtek RTL2838UHIDIR" -> the product, a maker other than Realtek in
 * front, else "vid:pid"); "" when none. */
std::string find_sdr(const std::string &dir);

} // namespace viz
