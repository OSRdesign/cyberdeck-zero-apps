/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas tests: the list of GPS receivers Port = Auto may open (core/gps_ids.cpp): known ids yes, every other serial
 * device (the owner's CH340 radar, ESP32 and LoRa boards, CP210x, FTDI, no id) no.
 */
#include "check.hpp"
#include "gps_ids.hpp"

#include <cstring>

using namespace atlas;

void test_gps_ids()
{
    /* known: u-blox (every product), Prolific PL2303 and PL2303GC */
    CHECK(gps_known_receiver(0x1546, 0x01a7));
    CHECK(gps_known_receiver(0x1546, 0x01a8));
    CHECK(gps_known_receiver(0x1546, 0x01a9));
    CHECK(gps_known_receiver(0x1546, 0x0000));
    CHECK(gps_known_receiver(0x1546, 0xFFFF));
    CHECK(gps_known_receiver(0x067b, 0x2303));
    CHECK(gps_known_receiver(0x067b, 0x23a3));
    CHECK(std::strcmp(gps_receiver_name(0x1546, 0x01a8), "u-blox GNSS receiver") == 0);
    CHECK(std::strstr(gps_receiver_name(0x067b, 0x2303), "Prolific") != nullptr);
    /* unknown: never opened in Auto */
    CHECK(!gps_known_receiver(0x1a86, 0x7523));   /* CH340: the mmWave radar */
    CHECK(!gps_known_receiver(0x1a86, 0x55d4));   /* CH9102 */
    CHECK(!gps_known_receiver(0x1a86, 0x0000));   /* 1a86:* */
    CHECK(!gps_known_receiver(0x10c4, 0xea60));   /* CP210x: LoRa boards */
    CHECK(!gps_known_receiver(0x10c4, 0x0000));
    CHECK(!gps_known_receiver(0x0403, 0x6001));   /* FTDI */
    CHECK(!gps_known_receiver(0x0403, 0x6015));
    CHECK(!gps_known_receiver(0x303a, 0x1001));   /* Espressif (ESP32-S3 native USB) */
    CHECK(!gps_known_receiver(0x303a, 0x0002));
    CHECK(!gps_known_receiver(0x239a, 0x8029));   /* an Adafruit / nRF board */
    CHECK(!gps_known_receiver(0x067b, 0x2304));   /* another Prolific product */
    CHECK(!gps_known_receiver(0x067b, 0x0000));
    CHECK(!gps_known_receiver(0x0000, 0x0000));   /* no USB id (a UART, a pty) */
    CHECK(!gps_known_receiver(0x1547, 0x01a8));   /* a vendor id next to u-blox's */
    CHECK(std::strcmp(gps_receiver_name(0x1a86, 0x7523), "") == 0);
}
