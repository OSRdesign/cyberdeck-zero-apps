/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the device knowledge table (see devices.hpp).
 *
 * One line per device. Add a device by adding a line; keep the hint short: it is the value of an Info row of the port
 * page, about 22 characters fit at 640x480 (tests/test_devices.cpp checks it). The name is the left of a ports-page row
 * and about 17 characters of it fit at 640x480 (the roller ends a longer one with dots, never the port name at its
 * right): keep it short, put the detail in the hint. Sources: the vendors' USB ids as Linux lists them (usb.ids, the usb-serial and cdc-acm
 * drivers) and the boards' documentation. Speeds:
 *   - USB-serial chips (FTDI, CP210x, CH340, PL2303): the chip passes the board's UART through, so the speed is the
 *     board's: 115200 for most dev boards (ESP32, Meshtastic nodes, routers), 9600 for older devices;
 *   - the ESP8266 boot ROM prints its messages at 74880 (then the app at its own speed, usually 115200);
 *   - Arduino sketches mostly start with Serial.begin(9600); the Uno bootloader runs at 115200, old Nano at 57600;
 *   - GPS receivers send NMEA at 9600 (some u-blox modules 38400);
 *   - native USB ports (Pico, Teensy, ESP32-S3 USB, Flipper Zero) ignore the speed.
 */
#include "devices.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace sc {
namespace {

constexpr int A = kAnyProduct;

const std::vector<DeviceInfo> kTable = {
    /* ---- FTDI (0403) */
    {0x0403, 0x6001, "", "FT232R USB-serial", "FTDI", Family::Bridge, 115200, "cables and routers"},
    {0x0403, 0x6010, "", "FT2232 dual UART", "FTDI", Family::Bridge, 115200, "JTAG + UART adapters"},
    {0x0403, 0x6011, "", "FT4232H quad UART", "FTDI", Family::Bridge, 115200, "multi-port adapters"},
    {0x0403, 0x6014, "", "FT232H USB-serial", "FTDI", Family::Bridge, 115200, "serial / JTAG adapters"},
    {0x0403, 0x6015, "", "FT231X USB-serial", "FTDI", Family::Bridge, 115200, "cables and boards"},
    {0x0403, A, "", "FTDI USB-serial", "FTDI", Family::Bridge, 115200, "USB-serial cables"},

    /* ---- Prolific (067b) */
    {0x067b, 0x2303, "", "PL2303 USB-serial", "Prolific", Family::Bridge, 115200, "cables, GPS, consoles"},
    {0x067b, 0x23a3, "", "PL2303GC serial", "Prolific", Family::Bridge, 115200, "USB-serial cables"},
    {0x067b, A, "", "Prolific serial", "Prolific", Family::Bridge, 115200, "USB-serial cables"},

    /* ---- Silicon Labs (10c4): CP2102 / CP2102N / CP2104 on ESP32 boards and Meshtastic nodes (Heltec, LilyGO, RAK) */
    {0x10c4, 0xea60, "", "CP210x USB-serial", "Silicon Labs", Family::EspBridge, 115200, "ESP32, Meshtastic"},
    {0x10c4, 0xea70, "", "CP2105 dual UART", "Silicon Labs", Family::EspBridge, 115200, "dual UART adapters"},
    {0x10c4, 0xea71, "", "CP2108 quad UART", "Silicon Labs", Family::Bridge, 115200, "multi-port adapters"},
    {0x10c4, A, "", "Silabs USB-serial", "Silicon Labs", Family::Bridge, 115200, "adapters and boards"},

    /* ---- WCH (1a86): CH340 on ESP8266 boards and Arduino clones, CH9102 / CH343 on newer ESP32 boards */
    {0x1a86, 0x7523, "", "CH340 USB-serial", "WCH", Family::EspBridge, 115200, "ESP8266, Nano clones"},
    {0x1a86, 0x5523, "", "CH341 USB-serial", "WCH", Family::Bridge, 115200, "USB-serial adapters"},
    {0x1a86, 0x55d4, "", "CH9102 USB-serial", "WCH", Family::EspBridge, 115200, "ESP32, LilyGO boards"},
    {0x1a86, 0x55d3, "", "CH343 USB-serial", "WCH", Family::EspBridge, 115200, "ESP32 boards"},
    {0x1a86, A, "", "WCH USB-serial", "WCH", Family::Bridge, 115200, "adapters and boards"},

    /* ---- Espressif native USB (303a) */
    {0x303a, 0x1001, "", "ESP32 USB-JTAG", "Espressif", Family::Esp, 115200, "S3/C3 USB-Serial/JTAG"},
    {0x303a, 0x4001, "", "ESP32 native USB", "Espressif", Family::Esp, 115200, "ESP32-S2/S3, CDC"},
    {0x303a, A, "", "Espressif USB", "Espressif", Family::Esp, 115200, "ESP32 own USB"},

    /* ---- Arduino (2341, and 2a03 of the former arduino.org) */
    {0x2341, 0x0043, "", "Arduino Uno", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x0001, "", "Arduino Uno", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x0042, "", "Arduino Mega 2560", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x0010, "", "Arduino Mega 2560", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x0058, "", "Arduino Nano Every", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x8036, "", "Arduino Leonardo", "Arduino", Family::Arduino, 9600, "sketches, native USB"},
    {0x2341, 0x003d, "", "Due (programming)", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x003e, "", "Due (native)", "Arduino", Family::Arduino, 9600, "sketches (SerialUSB)"},
    {0x2341, 0x0069, "", "Uno R4 Minima", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, 0x1002, "", "Uno R4 WiFi", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2341, A, "", "Arduino board", "Arduino", Family::Arduino, 9600, "Arduino sketches"},
    {0x2a03, A, "", "Arduino board", "Arduino", Family::Arduino, 9600, "Arduino sketches"},

    /* ---- Raspberry Pi (2e8a): Pico SDK stdio and MicroPython on USB, the Debug Probe's UART */
    {0x2e8a, 0x000a, "", "Raspberry Pi Pico", "Raspberry Pi", Family::NativeUsb, 115200, "Pico SDK programs"},
    {0x2e8a, 0x0005, "", "Pico MicroPython", "Raspberry Pi", Family::NativeUsb, 115200, "MicroPython prompt"},
    {0x2e8a, 0x000c, "", "Pi Debug Probe", "Raspberry Pi", Family::Debugger, 115200, "a target's UART"},
    {0x2e8a, A, "", "RP2040 board", "Raspberry Pi", Family::NativeUsb, 115200, "Pico, RP2040 boards"},

    /* ---- maker boards on their own USB (CircuitPython, Arduino cores) */
    {0x239a, 0x8029, "", "nRF52840 board", "Adafruit / RAKwireless", Family::NativeUsb, 115200, "Meshtastic RAK4631"},
    {0x239a, A, "", "Adafruit board", "Adafruit", Family::NativeUsb, 115200, "CircuitPython, Arduino"},
    {0x1b4f, A, "", "SparkFun board", "SparkFun", Family::NativeUsb, 115200, "Pro Micro, sketches"},
    {0x2886, A, "", "Seeed board", "Seeed Studio", Family::NativeUsb, 115200, "XIAO and Wio boards"},
    {0x16c0, 0x0483, "", "Teensy", "PJRC", Family::NativeUsb, 115200, "Teensy sketches"},

    /* ---- STMicroelectronics (0483): ST-LINK virtual COM ports, STM32 USB CDC, Flipper Zero */
    {0x0483, 0x374b, "", "ST-LINK/V2-1 VCP", "STMicroelectronics", Family::Debugger, 115200, "Nucleo, Discovery"},
    {0x0483, 0x3752, "", "ST-LINK/V2-1 VCP", "STMicroelectronics", Family::Debugger, 115200, "Nucleo, Discovery"},
    {0x0483, 0x374e, "", "STLINK-V3 VCP", "STMicroelectronics", Family::Debugger, 115200, "Nucleo-144 boards"},
    {0x0483, 0x5740, "flipper", "Flipper Zero", "Flipper Devices", Family::Cli, 115200, "the Flipper CLI"},
    {0x0483, 0x5740, "", "STM32 USB CDC", "STMicroelectronics", Family::NativeUsb, 115200, "STM32 firmware"},

    /* ---- debug probes */
    {0x0d28, 0x0204, "", "DAPLink micro:bit", "Arm", Family::Debugger, 115200, "micro:bit, mbed"},
    {0x1366, A, "", "SEGGER J-Link VCP", "SEGGER", Family::Debugger, 115200, "a target's UART"},

    /* ---- GPS receivers (u-blox USB) */
    {0x1546, 0x01a7, "", "u-blox 7 GPS", "u-blox", Family::Gnss, 9600, "GPS: NMEA sentences"},
    {0x1546, 0x01a8, "", "u-blox 8 GPS", "u-blox", Family::Gnss, 9600, "GPS: NMEA sentences"},
    {0x1546, 0x01a9, "", "u-blox 9 GPS", "u-blox", Family::Gnss, 9600, "GPS: NMEA sentences"},
    {0x1546, A, "", "u-blox GPS", "u-blox", Family::Gnss, 9600, "GPS: NMEA sentences"},

    /* ---- network devices */
    {0x05a6, 0x0009, "", "Cisco USB console", "Cisco", Family::Console, 9600, "a switch console"},
    {0x05a6, A, "", "Cisco console", "Cisco", Family::Console, 9600, "a switch console"},
};

const char *family_reason(Family family)
{
    switch (family) {
    case Family::EspBridge: return "ESP32 boards use 115200; ESP8266 ROM: 74880";
    case Family::Bridge: return "Boards and routers: 115200; old switches: 9600";
    case Family::Esp: return "ESP32 own USB: the speed does not matter";
    case Family::Arduino: return "Most Arduino sketches use Serial.begin(9600)";
    case Family::NativeUsb: return "Board's own USB: the speed does not matter";
    case Family::Debugger: return "Use the speed of the target's firmware";
    case Family::Gnss: return "GPS sends NMEA at 9600 (some at 38400)";
    case Family::Console: return "Cisco and most switch consoles use 9600";
    case Family::Cli: return "A USB command line: type help, then Enter";
    case Family::Unknown: break;
    }
    return "The most common speed; garbled? Find the speed";
}

} // namespace

const std::vector<DeviceInfo> &device_table()
{
    return kTable;
}

bool contains_nocase(const std::string &text, const std::string &part)
{
    if (part.empty()) return true;
    if (part.size() > text.size()) return false;
    for (std::size_t i = 0; i + part.size() <= text.size(); ++i) {
        std::size_t k = 0;
        while (k < part.size() && std::tolower(static_cast<unsigned char>(text[i + k])) ==
                                      std::tolower(static_cast<unsigned char>(part[k])))
            ++k;
        if (k == part.size()) return true;
    }
    return false;
}

const DeviceInfo *lookup_device(std::uint16_t vid, std::uint16_t pid, const std::string &product)
{
    const DeviceInfo *exact = nullptr, *vendor = nullptr;
    for (const DeviceInfo &d : kTable) {
        if (d.vid != vid) continue;
        if (d.pid == static_cast<int>(pid)) {
            if (d.product[0]) {
                if (contains_nocase(product, d.product)) return &d; /* the most specific: product string too */
                continue;
            }
            if (!exact) exact = &d;
        } else if (d.pid == kAnyProduct && !vendor) {
            vendor = &d;
        }
    }
    return exact ? exact : vendor;
}

Recommendation recommend(const DeviceInfo *info, bool onboard)
{
    Recommendation r;
    r.baud = onboard || !info ? kDefaultBaud : info->baud;
    r.reason = onboard ? "The usual Pi console speed on GPIO 14/15" : family_reason(info ? info->family : Family::Unknown);
    r.why = std::to_string(r.baud) + " " + r.frame + ": " + r.reason;
    return r;
}

std::string usb_id_text(std::uint16_t vid, std::uint16_t pid)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

std::string chip_variant(std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd, int interfaces,
                         const std::string &product)
{
    if (vid == 0x0403 && bcd) { /* Linux ftdi_sio's ftdi_determine_type() */
        if (interfaces > 1) {
            if (bcd == 0x0800) return "FT4232H";
            if (bcd == 0x0700) return "FT2232H";
            if (bcd == 0x3600) return "FT4232HA";
            return "FT2232C/D";
        }
        if (bcd < 0x0200) return "FTDI SIO";
        if (bcd < 0x0400) return "FT8U232AM";
        if (bcd < 0x0600) return "FT232BM";
        if (bcd < 0x0900) return "FT232R";
        if (bcd < 0x1000) return "FT232H";
        return "FT-X (FT231X / FT230X)";
    }
    if (vid == 0x1a86) {
        switch (pid) {
        case 0x7523: return "CH340";
        case 0x5523: return "CH341";
        case 0x55d4: return "CH9102";
        case 0x55d3: return "CH343";
        case 0x55d2: return "CH342";
        default: return std::string();
        }
    }
    if (vid == 0x10c4) {
        for (const char *part : {"CP2102N", "CP2104", "CP2109", "CP2105", "CP2108", "CP2102"})
            if (contains_nocase(product, part)) return part;
        return std::string();
    }
    if (vid == 0x303a && pid == 0x1001) return "ESP USB-Serial/JTAG";
    if (vid == 0x067b && pid == 0x23a3) return "PL2303GC";
    return std::string();
}

std::string bcd_text(std::uint16_t bcd)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x.%02x", bcd >> 8, bcd & 0xFF);
    return buf;
}

std::string usb_speed_text(const std::string &speed)
{
    if (speed.empty()) return std::string();
    const double mbit = std::atof(speed.c_str());
    char buf[32];
    if (mbit >= 1000) std::snprintf(buf, sizeof(buf), "%g Gbit/s", mbit / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%s Mbit/s", speed.c_str());
    return buf;
}

bool boot_mode(std::uint16_t vid, std::uint16_t pid, const std::string &product, BootMode *out)
{
    std::string chip;
    if (vid == 0x2e8a && pid == 0x0003) chip = "RP2040";
    if (vid == 0x2e8a && pid == 0x000f) chip = "RP2350";
    if (!chip.empty()) {
        if (out) {
            out->title = chip + " in BOOTSEL mode";
            out->hint = "A USB drive, not a serial port: replug it without BOOT";
            out->advice = chip + " in BOOTSEL mode (a USB drive, not a serial port): unplug and replug without the "
                                 "BOOT button";
        }
        return true;
    }
    if (contains_nocase(product, "UF2")) {
        if (out) {
            out->title = "UF2 boot loader";
            out->hint = "A USB drive, not a serial port: press reset once";
            out->advice = (product.empty() ? std::string("A board") : product) +
                          " is in its UF2 boot loader (a USB drive, not a serial port): press its reset button once, "
                          "or unplug and replug it, to run its program";
        }
        return true;
    }
    return false;
}

} // namespace sc
