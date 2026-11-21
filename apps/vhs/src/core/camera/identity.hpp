/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a camera's USB identity, read from sysfs (brief 3.4 and 5.5). The camera is remembered by this identity
 * (vid:pid + serial, else the USB device path), never by /dev/videoN (D-15). The host controller's driver of the bus it
 * is on tells the dwc_otg boards (Pi 3A+, Zero 2 W: a camera whose switch moved does not come back, C12) from the xhci
 * ones. Nothing here writes to sysfs or touches the bus (C12: no unbind, no reset, no authorized toggles).
 *
 * sys_root "" = the real system; a folder = a fixture tree holding sys/class/video4linux/... (tests). LVGL-free.
 */
#pragma once

#include <string>

namespace vhs {

struct CamIdentity {
    std::string vid, pid;      /* "a108", "2240" (idVendor, idProduct) */
    std::string serial;        /* the USB serial string, "" when the device has none */
    std::string devpath;       /* the USB device's sysfs name, "1-1.2" (bus 1, ports 1.2) */
    std::string manufacturer;  /* USB strings, for EXIF Make / Model (M2) and the page's note */
    std::string product;
    std::string hc_driver;     /* the host controller's driver: "xhci_hcd", "xhci-hcd", "dwc_otg", "dwc2"; "" unknown */
    std::string stable;        /* 0.1.2: for a node that is not a USB device (a platform / ISP camera): "sys:<sysfs device path>" */

    bool valid() const { return !vid.empty() && !pid.empty(); }
    /* "a108:2240:SERIAL", else "a108:2240@1-1.2"; "" when not valid. The settings key camera_id. */
    std::string key() const;
    bool same(const CamIdentity &o) const { return valid() && key() == o.key(); }
    /* 0.1.2: the id the camera chooser and the settings use: key() for a USB camera, else `stable`, else "" (D-15: never
     * /dev/videoN). */
    std::string stable_key() const { return valid() ? key() : stable; }
    bool known() const { return valid() || !stable.empty(); }
    bool same_camera(const CamIdentity &o) const { return known() && stable_key() == o.stable_key(); }
};

/* The V4L2 node's name: "video3" for /dev/video3, for a by-id link to it (resolved), or for "video3" itself. */
std::string video_name_of(const std::string &node_path);

/* The USB device directory (holding idVendor) above /sys/class/video4linux/<video_name>/device, "" when the node is
 * not on USB. */
std::string usb_device_dir(const std::string &sys_root, const std::string &video_name);

/* The identity of a V4L2 node (invalid when it is not a USB device). */
CamIdentity identity_for_node(const std::string &sys_root, const std::string &node_path);

/* A non-USB node's stable id (0.1.2): "sys:" + the part of its sysfs device path after "/devices/" (the platform
 * device, e.g. "sys:platform/soc/fe801000.csi"), "" when the node has no device link. */
std::string sysfs_node_key(const std::string &sys_root, const std::string &video_name);

/* The driver of the host controller above a USB device directory: walk up to the root hub (the topmost directory
 * holding idVendor), then its parent's driver symlink. "" when unknown. */
std::string host_controller_driver(const std::string &usb_dev_dir);

/* A bus that does not re-enumerate a camera whose switch moved (dwc_otg, dwc2): the NoCamera page's replug hint. */
bool bus_needs_replug(const std::string &hc_driver);

/* The driver bound to the node's sysfs device ("uvcvideo"), "" when unknown. */
std::string node_driver(const std::string &sys_root, const std::string &video_name);

} // namespace vhs
