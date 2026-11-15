/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: which ALSA card is the camera's microphone (brief 5.4). The card whose sysfs device
 * (/sys/class/sound/cardN/device, a USB interface) has the same USB device above it as the V4L2 node
 * (/sys/class/video4linux/videoN/device): the FineCam's CM969 and the C270's separate "USB Audio" card alike. It is
 * opened as hw:CARD=<id> with the id from /proc/asound/cardN/id, never by its index (plan 3.6).
 *
 * sys_root "" = the real system; a folder = a fixture tree holding sys/... and proc/asound/... (tests). LVGL-free.
 */
#pragma once

#include "alsa/alsa_api.hpp"

#include <string>

namespace vhs {

struct MicCard {
    bool found = false;
    CardInfo card;          /* index (this boot's, for the log only) and id; capture = true */
    std::string usb_dir;    /* the USB device both hang off */
};

/* camera_usb_dir: identity.hpp usb_device_dir() of the camera's node. */
MicCard camera_mic(const std::string &sys_root, const std::string &camera_usb_dir);

/* Without sysfs (the fake camera): the capture card on the camera's USB port (CardInfo::usb_port == devpath). */
MicCard camera_mic_by_port(AlsaApi &api, const std::string &devpath);

} // namespace vhs
