/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the device knowledge table (core/devices).
 */
#include "check.hpp"

#include "devices.hpp"

#include <cstring>
#include <set>
#include <tuple>

using namespace sc;

void test_devices()
{
    // every entry is complete and its hint fits a row
    std::set<std::tuple<int, int, std::string>> keys;
    for (const DeviceInfo &d : device_table()) {
        CHECK(d.name && d.name[0]);
        CHECK(d.maker && d.maker[0]);
        CHECK(std::strlen(d.name) <= 18); /* the left of a ports-page row: about 17 characters fit at 640x480 */
        CHECK(d.hint && d.hint[0] && std::strlen(d.hint) <= 22); /* one Info row value at 640x480 */
        CHECK(d.baud >= 1200 && d.baud <= 921600);
        CHECK(keys.insert(std::make_tuple(d.vid, d.pid, std::string(d.product))).second); /* no duplicate */
    }
    CHECK(device_table().size() >= 40);

    // the chips of the brief
    const struct {
        std::uint16_t vid, pid;
        const char *name;
        int baud;
    } cases[] = {
        {0x0403, 0x6001, "FT232R USB-serial", 115200},   {0x0403, 0x6010, "FT2232 dual UART", 115200},
        {0x0403, 0x6014, "FT232H USB-serial", 115200},   {0x0403, 0x6015, "FT231X USB-serial", 115200},
        {0x067b, 0x2303, "PL2303 USB-serial", 115200},   {0x067b, 0x23a3, "PL2303GC serial", 115200},
        {0x10c4, 0xea60, "CP210x USB-serial", 115200},   {0x10c4, 0xea70, "CP2105 dual UART", 115200},
        {0x1a86, 0x7523, "CH340 USB-serial", 115200},    {0x1a86, 0x5523, "CH341 USB-serial", 115200},
        {0x1a86, 0x55d4, "CH9102 USB-serial", 115200},   {0x2341, 0x0043, "Arduino Uno", 9600},
        {0x2341, 0x0042, "Arduino Mega 2560", 9600},     {0x2341, 0x0058, "Arduino Nano Every", 9600},
        {0x2341, 0x8036, "Arduino Leonardo", 9600},      {0x2341, 0x003d, "Due (programming)", 9600},
        {0x303a, 0x1001, "ESP32 USB-JTAG", 115200}, {0x303a, 0x4001, "ESP32 native USB", 115200},
        {0x2e8a, 0x000a, "Raspberry Pi Pico", 115200},   {0x2e8a, 0x0005, "Pico MicroPython", 115200},
        {0x0483, 0x374b, "ST-LINK/V2-1 VCP", 115200}, {0x0483, 0x3752, "ST-LINK/V2-1 VCP", 115200},
        {0x0483, 0x374e, "STLINK-V3 VCP", 115200}, {0x16c0, 0x0483, "Teensy", 115200},
        {0x1546, 0x01a7, "u-blox 7 GPS", 9600}, {0x1546, 0x01a8, "u-blox 8 GPS", 9600},
        {0x05a6, 0x0009, "Cisco USB console", 9600},
    };
    for (const auto &c : cases) {
        const DeviceInfo *d = lookup_device(c.vid, c.pid);
        CHECK(d != nullptr);
        if (!d) continue;
        CHECK_EQ(std::string(d->name), std::string(c.name));
        CHECK_EQ(d->baud, c.baud);
    }

    // vendor-wide entries: a product the table does not list still gets its maker's entry
    const DeviceInfo *adafruit = lookup_device(0x239a, 0x80f4);
    CHECK(adafruit && std::string(adafruit->maker) == "Adafruit" && adafruit->pid == kAnyProduct);
    const DeviceInfo *ftdi = lookup_device(0x0403, 0x1234);
    CHECK(ftdi && std::string(ftdi->name) == "FTDI USB-serial");
    CHECK(lookup_device(0x1b4f, 0x9206) && lookup_device(0x2886, 0x802f) && lookup_device(0x2a03, 0x0043));
    CHECK(lookup_device(0x2341, 0x9999) && lookup_device(0x2341, 0x9999)->family == Family::Arduino);

    // the product string tells a Flipper Zero from any STM32 virtual COM port (same 0483:5740)
    const DeviceInfo *flipper = lookup_device(0x0483, 0x5740, "Flipper Kirelo Virtual ComPort");
    CHECK(flipper && std::string(flipper->name) == "Flipper Zero" && flipper->family == Family::Cli);
    const DeviceInfo *stm = lookup_device(0x0483, 0x5740, "STM32 Virtual ComPort");
    CHECK(stm && std::string(stm->name) == "STM32 USB CDC");
    CHECK(lookup_device(0x0483, 0x5740) == stm);

    // unknown devices
    CHECK(lookup_device(0x1234, 0x5678) == nullptr);
    const Recommendation unknown = recommend(nullptr, false);
    CHECK_EQ(unknown.baud, 115200);
    CHECK_EQ(unknown.frame, std::string("8N1"));
    CHECK(unknown.why.compare(0, 11, "115200 8N1:") == 0);
    CHECK(unknown.why == "115200 8N1: " + unknown.reason);
    for (const Family f : {Family::Bridge, Family::EspBridge, Family::Esp, Family::Arduino, Family::NativeUsb,
                           Family::Debugger, Family::Gnss, Family::Console, Family::Cli, Family::Unknown}) {
        DeviceInfo d{0, 0, "", "x", "x", f, 9600, "x"};
        CHECK(!recommend(&d, false).reason.empty() && recommend(&d, false).reason.size() <= 48); /* two status lines */
    }

    // the family hints of the brief
    const Recommendation esp = recommend(lookup_device(0x10c4, 0xea60), false);
    CHECK(esp.why.find("74880") != std::string::npos && esp.why.find("ESP32") != std::string::npos);
    CHECK(std::string(lookup_device(0x1a86, 0x7523)->hint).find("ESP8266") != std::string::npos);
    const Recommendation gps = recommend(lookup_device(0x1546, 0x01a8), false);
    CHECK(gps.baud == 9600 && gps.why.find("NMEA") != std::string::npos);
    const Recommendation uno = recommend(lookup_device(0x2341, 0x0043), false);
    CHECK(uno.baud == 9600 && uno.why.find("Serial.begin") != std::string::npos);
    const Recommendation router = recommend(lookup_device(0x0403, 0x6001), false);
    CHECK(router.why.find("9600") != std::string::npos && router.why.find("routers") != std::string::npos);
    CHECK(recommend(flipper, false).why.find("help") != std::string::npos);
    const Recommendation uart = recommend(nullptr, true);
    CHECK(uart.baud == 115200 && uart.why.find("GPIO 14/15") != std::string::npos);

    CHECK_EQ(usb_id_text(0x10c4, 0xea60), std::string("10c4:ea60"));
    CHECK_EQ(usb_id_text(0x0403, 0x6001), std::string("0403:6001"));
    CHECK(contains_nocase("Flipper Zero ComPort", "FLIPPER"));
    CHECK(!contains_nocase("Flip", "flipper"));
    CHECK(contains_nocase("anything", ""));
}
