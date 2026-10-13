/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: finding the serial ports and what is plugged into them (no LVGL; tests/test_ports.cpp runs it on a
 * fake sysfs tree made by fake_sys.hpp).
 *
 * Ports: /dev/ttyUSB* and /dev/ttyACM* (USB), and on request the on-board UARTs /dev/ttyAMA* and /dev/ttyS*. A tty is
 * listed when /sys/class/tty/<tty>/device exists (a real port, not a virtual console) and its /dev node exists; a
 * ttyS* whose sysfs "type" is 0 is an 8250 slot with no UART behind it (PCs list 4 or 32 of them) and is skipped; the
 * target of /dev/serial1 is the Raspberry Pi's Bluetooth UART and is skipped (opening it would cut the Bluetooth
 * keyboard). The target of /dev/serial0 is the UART on GPIO 14/15.
 *
 * Identification: from /sys/class/tty/<tty>/device up to the USB device directory (the first one with idVendor):
 * idVendor, idProduct, manufacturer, product, serial; the interface number (bInterfaceNumber) on the way; the bound
 * driver (the basename of device/driver: cp210x, ftdi_sio, ch341, pl2303, cdc_acm). /dev/serial/by-id gives the stable
 * name the settings are kept under.
 *
 * Every path is under Roots (default / ): the tests and the demonstration world (demo.hpp) point them at a fake tree.
 */
#pragma once

#include "devices.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct Roots {
    std::string sys = "/sys";
    std::string dev = "/dev";
};

struct PortInfo {
    std::string tty;          /* "ttyUSB0" */
    std::string path;         /* "<dev>/ttyUSB0" */
    std::string by_id;        /* the /dev/serial/by-id link name, "" when none */
    bool usb = false;
    bool onboard = false;     /* ttyAMA* / ttyS* */
    bool gpio = false;        /* the target of /dev/serial0: GPIO 14/15 */
    std::uint16_t vid = 0, pid = 0;
    std::string manufacturer, product, serial, driver, interface;
    const DeviceInfo *known = nullptr;
    std::string id;           /* the stable key of its settings (port_id) */
    std::string title;        /* the port list's title (port_title) */
    Recommendation rec;
};

/* Every port, USB first (by tty name, ttyUSB2 before ttyUSB10), then the on-board UARTs when include_onboard. */
std::vector<PortInfo> scan_ports(const Roots &roots, bool include_onboard);

/* The settings key: the by-id name, else "usb-<vid>:<pid>[-<serial>]-if<interface>" for USB, else the tty name. */
std::string port_id(const PortInfo &port);
/* The friendly title: the table's name for a known product, the USB product string for an unknown one or a
 * vendor-wide entry, "On-board UART (GPIO 14/15)" for serial0's UART, else the tty name. */
std::string port_title(const PortInfo &port);

/* The /dev path of the port with this id now ("" when it is not there): follows a board that comes back on another
 * ttyUSB number. */
std::string resolve_port(const Roots &roots, const std::string &id);

/* Same ports, same names (the list does not need redrawing). */
bool same_port(const PortInfo &a, const PortInfo &b);

/* ---------------------------------------------------------------- permissions */

struct GroupState {
    bool root = false;     /* uid 0: no group needed */
    bool active = false;   /* dialout is one of this process's groups */
    bool listed = false;   /* the user is a member in the group database (active after a new login) */
    std::string user;      /* login name, "" when unknown */
};

/* The process's dialout state (getgroups, the group database). */
GroupState dialout_state();

/* A serial port needs no fix: root, or dialout active. */
inline bool group_ok(const GroupState &g) { return g.root || g.active; }

struct Problem {
    std::string title;  /* "No permission for ttyUSB0" */
    std::string text;   /* what is wrong and the exact fix */
};

/* What to tell the user when opening `tty` failed with errno `err`. */
Problem open_problem(int err, const GroupState &groups, const std::string &tty);
/* The fix for a user not in dialout (the ports page's row and its dialog). */
Problem group_problem(const GroupState &groups);

/* The exact command the user types. */
inline const char *dialout_fix_command() { return "sudo usermod -aG dialout $USER"; }

} // namespace sc
