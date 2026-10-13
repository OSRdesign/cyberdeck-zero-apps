/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: what the app knows about USB serial devices (no LVGL; unit-tested by tests/test_devices.cpp).
 *
 * A plain table (devices.cpp) maps a USB vendor:product id to a name, a maker, a family, the speed to recommend and a
 * one-line "Typically:" hint. The lookup takes the most specific entry: the exact product with a matching product
 * string, then the exact product, then the vendor-wide entry (pid kAnyProduct). A device the table does not know still
 * works: the port list names it from its USB product string, and recommend() falls back to 115200 8N1.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

constexpr int kAnyProduct = -1;
constexpr int kDefaultBaud = 115200;

/* What kind of thing it is: chooses the recommendation's explanation. */
enum class Family {
    Bridge,     /* a USB-serial chip on a cable or a board (FTDI, CP210x, CH340, PL2303) */
    EspBridge,  /* a bridge chip that is mostly on ESP32 / ESP8266 boards (CP210x, CH340, CH9102) */
    Esp,        /* Espressif's own USB (ESP32-S2/S3/C3/C6 native USB) */
    Arduino,    /* an official Arduino board */
    NativeUsb,  /* a microcontroller's own USB (Pico, Teensy, CircuitPython boards): the speed does not matter */
    Debugger,   /* a debug probe's virtual COM port (ST-LINK, J-Link, DAPLink): bridges to the target's UART */
    Gnss,       /* a GPS / GNSS receiver: NMEA text */
    Console,    /* a network device's console cable (Cisco) */
    Cli,        /* a gadget with a command line on USB (Flipper Zero) */
    Unknown,
};

struct DeviceInfo {
    std::uint16_t vid;
    int pid;                /* kAnyProduct: every product of the vendor */
    const char *product;    /* "" or a substring the USB product string must contain (case-insensitive) */
    const char *name;       /* the port list's title */
    const char *maker;
    Family family;
    int baud;               /* recommended speed */
    const char *hint;       /* "Typically: ..." without the word; at most 22 characters (one Info row at 640x480) */
};

/* The whole table (for tests and documentation). */
const std::vector<DeviceInfo> &device_table();

/* The best entry for this device, or nullptr. product: the USB product string ("" when absent). */
const DeviceInfo *lookup_device(std::uint16_t vid, std::uint16_t pid, const std::string &product = std::string());

/* What the port page recommends. */
struct Recommendation {
    int baud = kDefaultBaud;
    std::string frame = "8N1";
    std::string reason; /* one short sentence (the port page's status line): "ESP32 boards use 115200; ESP8266 ROM: 74880" */
    std::string why;    /* "115200 8N1: " + reason */
};

/* The recommendation for a known device (nullptr: an unknown USB device; onboard: a board's own UART). */
Recommendation recommend(const DeviceInfo *info, bool onboard);

/* "10c4:ea60" */
std::string usb_id_text(std::uint16_t vid, std::uint16_t pid);

/* Case-insensitive substring test (ASCII). */
bool contains_nocase(const std::string &text, const std::string &part);

} // namespace sc
