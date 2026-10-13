/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: a fake /sys and /dev tree with serial ports in it, laid out as Linux lays them out (the unit tests
 * and the demonstration world use it; the app never writes outside its temporary folder with it):
 *   sys/devices/usb1/1-N/{idVendor,idProduct,manufacturer,product,serial,bcdDevice,bNumInterfaces,speed}
 *                                                                              the USB device
 *   sys/bus/usb/devices/1-N -> the USB device
 *   sys/devices/usb1/1-N/1-N:1.I/bInterfaceNumber                              its interface
 *   .../1-N:1.I/ttyUSB0/driver -> sys/bus/usb-serial/drivers/<driver>          a usb-serial port (ttyUSB)
 *   .../1-N:1.I/driver -> sys/bus/usb/drivers/cdc_acm                          an ACM interface (ttyACM)
 *   sys/class/tty/<tty>/device -> the port's device directory; type (on-board UARTs)
 *   dev/<tty>                    a link to a real terminal (a pseudo-terminal of the demo), or a plain file
 *   dev/serial/by-id/<name> -> ../../<tty>, dev/serial0 -> <tty>
 */
#pragma once

#include "ports.hpp"

#include <cstdint>
#include <string>

namespace sc {
namespace fake {

struct UsbTty {
    std::string tty;          /* ttyUSB0 or ttyACM0 */
    std::uint16_t vid = 0, pid = 0;
    std::string manufacturer, product, serial;
    std::string driver;       /* cp210x, ftdi_sio, ch341, cdc_acm ... */
    std::string interface = "00";
    std::string by_id;        /* "" for none */
    std::string node_target;  /* "" : dev/<tty> is a plain file with node_mode; else a link to this path */
    unsigned node_mode = 0666;
    int usb_port = 1;         /* the USB device is 1-<usb_port> */
    std::uint16_t bcd = 0;    /* bcdDevice (0: not written) */
    int interfaces = 1;       /* bNumInterfaces */
    std::string speed = "12"; /* Mbit/s */
};

/* A USB device with no tty (a board in its boot loader: a USB drive). */
bool add_usb_device(const Roots &roots, const UsbTty &device);

/* sys/ and dev/ under base (created). */
Roots make_roots(const std::string &base);
bool add_usb_tty(const Roots &roots, const UsbTty &port);
/* An on-board UART: class/tty/<tty>/device and type; node_target as for UsbTty ("" a plain file). */
bool add_onboard_tty(const Roots &roots, const std::string &tty, const std::string &driver, int type,
                     const std::string &node_target);
/* A tty with no device behind it (a virtual console: never listed). */
bool add_virtual_tty(const Roots &roots, const std::string &tty);
/* dev/<alias> -> <tty> (serial0, serial1). */
bool add_alias(const Roots &roots, const std::string &alias, const std::string &tty);
/* The tty goes as at an unplug: its class entry, its /dev node and its by-id links. */
bool remove_tty(const Roots &roots, const std::string &tty);

} // namespace fake
} // namespace sc
