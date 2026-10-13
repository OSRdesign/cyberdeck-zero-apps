/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the classifier of "What is on the other end?" (core/identify) on the text samples of tests/fixtures (one
 * per firmware family: representative output in each firmware's real format, see the header of each file), on binary
 * frames built here with their real checksums, and of the identification's texts and its settings-file round trip.
 */
#include "check.hpp"

#include "console_core.hpp"
#include "fsutil.hpp"
#include "identify.hpp"
#include "settings.hpp"

#include <time.h>

#include <cstring>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

using namespace sc;

namespace {

/* A fixture: its "##" comment lines dropped, "\x1b" made the ESC byte, lines ended with CR LF as a UART sends them. */
std::string fixture(const char *name)
{
    bool ok = false;
    const std::string raw = fs::read_all(std::string(CDK_APP_DIR) + "/tests/fixtures/" + name, &ok);
    CHECK(ok);
    std::string out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t end = raw.find('\n', pos);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(pos, end - pos);
        pos = end + 1;
        if (line.compare(0, 2, "##") == 0) continue;
        for (std::size_t k; (k = line.find("\\x1b")) != std::string::npos;) line.replace(k, 4, "\x1b");
        out += line + "\r\n";
    }
    return out;
}

Identity run(const char *name, int baud, Family usb = Family::EspBridge, bool native = false)
{
    Context ctx;
    ctx.usb = usb;
    ctx.native_usb = native;
    return classify({Capture{fixture(name), baud}}, ctx);
}

std::int64_t now_ms_for_test()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string noise(std::size_t n, std::uint32_t seed)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1103515245u + 12345u;
        out += static_cast<char>((seed >> 16) & 0xFF);
    }
    return out;
}

void test_patterns()
{
    std::map<std::string, std::string> c;
    CHECK(match_pattern("Grbl {version} [", "Grbl 1.1h ['$' for help]", &c) && c["version"] == "1.1h");
    CHECK(match_pattern("^I (#) cpu_start: ", "I (122) cpu_start: Pro cpu", nullptr));
    CHECK(!match_pattern("^I (#) cpu_start: ", "xI (122) cpu_start: Pro cpu", nullptr));
    CHECK(!match_pattern("^I (#) cpu_start: ", "I (abc) cpu_start: Pro cpu", nullptr));
    c.clear();
    CHECK(match_pattern("MicroPython {version} on {date}; {board*} with {mcu*}$",
                        "MicroPython v1.22.2 on 2024-02-22; Raspberry Pi Pico with RP2040", &c));
    CHECK_EQ(c["version"], std::string("v1.22.2"));
    CHECK_EQ(c["date"], std::string("2024-02-22"));
    CHECK_EQ(c["board"], std::string("Raspberry Pi Pico"));
    CHECK_EQ(c["mcu"], std::string("RP2040"));
    c.clear();
    CHECK(match_pattern("ESP-IDF: {idf}", "I (147) app_init: ESP-IDF:          v5.1.2", &c) && c["idf"] == "v5.1.2");
    CHECK(match_pattern("^Switch\\#$", "Switch#", nullptr));
    CHECK(!match_pattern("^Switch\\#$", "Switch#x", nullptr));
    CHECK(match_pattern("a?c", "xxabcxx", nullptr) && !match_pattern("^a?c$", "abbc", nullptr));
    CHECK(match_pattern("TXT,#,#,#,FWVER={version*}\\*", "$GNTXT,01,01,02,FWVER=SPG 3.01*46", &c) &&
          c["version"] == "SPG 3.01");
    CHECK(match_pattern("E S H T /\\\\ S T / C", "//\\ E S H T /\\ S T / C", nullptr));
    c.clear();
    CHECK(!match_pattern("^{model} R{version#}$", "Call Ready", nullptr)); /* {name#}: a word that starts with a digit */
    CHECK(match_pattern("^{model} R{version#}$", "SIM800 R14.18", &c) && c["model"] == "SIM800" && c["version"] == "14.18");
    // a long line of near-matches stays fast (the matcher remembers the dead ends)
    const std::string long_line(400, 'a');
    CHECK(!match_pattern("a*a*a*a*b", long_line, nullptr));

    std::string t;
    CHECK(nmea_valid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", &t) && t == "GP");
    CHECK(!nmea_valid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48", nullptr));
    CHECK(!nmea_valid("$GPGGA,123519*", nullptr) && !nmea_valid("no sentence", nullptr));
    CHECK(nmea_valid("garbage before $GNTXT,01,01,02,u-blox AG - www.u-blox.com*4E", &t) && t == "GN");

    const std::vector<std::string> lines = clean_lines("\x1b[0;32mI (1) a: b\x1b[0m\r\nLoad 10%\rLoad 99%\r\nlast");
    CHECK(lines.size() == 3 && lines[0] == "I (1) a: b" && lines[1] == "Load 99%" && lines[2] == "last");
}

void test_crcs()
{
    CHECK_EQ(crc_x25("123456789"), static_cast<std::uint16_t>(0x6F91)); /* CRC-16/MCRF4XX check value */
    CHECK_EQ(crc16_klipper("123456789"), static_cast<std::uint16_t>(0x6F91));
    CHECK_EQ(crc24q("123456789"), static_cast<std::uint32_t>(0xCDE703)); /* CRC-24/LTE-A (RTCM's CRC-24Q) */
}

void test_fixtures()
{
    Identity id = run("esp32-idf-boot.txt", 115200);
    CHECK(id.kind == Kind::EspIdf);
    CHECK_EQ(id.chip, std::string("ESP32"));
    CHECK_EQ(id.firmware, std::string("ESP-IDF"));
    CHECK_EQ(id.version, std::string("v5.1.2"));
    CHECK(id.detail.find("project hello_world") != std::string::npos);
    CHECK(id.confidence >= 70 && id.baud == 115200);
    CHECK(!id.evidence.empty() && id.evidence.size() <= kEvidenceMax);
    for (const std::string &e : id.evidence) CHECK(e.size() <= kEvidenceChars);
    CHECK(!id.boot.empty() && id.boot[0] == "ets Jun  8 2016 00:22:57"); /* the boot lines, colour codes removed */
    bool clean = true;
    for (const std::string &l : id.boot) clean = clean && l.find('\x1b') == std::string::npos;
    CHECK(clean);

    id = run("esp32s3-rom-idf.txt", 115200);
    CHECK(id.kind == Kind::EspIdf && id.chip == "ESP32-S3" && id.version == "v5.1.2");

    id = run("esp32-download.txt", 115200);
    CHECK(id.kind == Kind::EspRom && id.chip == "ESP32");
    CHECK(id.detail.find("download mode") != std::string::npos);

    id = run("esp-idf-log.txt", 115200);
    CHECK(id.kind == Kind::EspIdf && id.confidence >= 35);

    id = run("esp8266-rom-74880.txt", 74880);
    CHECK(id.kind == Kind::Esp8266 && id.chip == "ESP8266" && id.baud == 74880);
    int baud = 0;
    std::string reason;
    CHECK(identity_recommendation(id, &baud, &reason) && baud == 74880 && reason.find("115200") != std::string::npos);

    id = run("arduino-esp32-debug.txt", 115200);
    CHECK(id.kind == Kind::ArduinoEsp32 && id.chip == "ESP32-S3" && id.board == "ESP32S3_DEV");
    CHECK_EQ(behind_text(id), std::string("ESP32-S3"));
    CHECK_EQ(id.firmware, std::string("Arduino-ESP32"));

    id = run("meshtastic-boot.txt", 115200);
    CHECK(id.kind == Kind::Meshtastic);
    CHECK_EQ(id.version, std::string("2.3.15.deb7c27"));
    CHECK_EQ(id.board, std::string("heltec-v3"));
    CHECK_EQ(id.chip, std::string("ESP32-S3")); /* from the ROM banner before it */

    id = run("gnss-ublox-m8.txt", 9600, Family::Bridge);
    CHECK(id.kind == Kind::Gnss && id.baud == 9600);
    CHECK_EQ(id.chip, std::string("u-blox NEO-M8N-0"));
    CHECK_EQ(id.firmware, std::string("NMEA"));
    CHECK_EQ(id.version, std::string("SPG 3.01"));
    CHECK(id.detail.find("GPS") != std::string::npos && id.detail.find("GLONASS") != std::string::npos);
    CHECK(id.confidence >= 70);
    CHECK(identity_recommendation(id, &baud, &reason) && baud == 9600 && reason == "GPS found: NMEA at 9600");

    id = run("gnss-mtk.txt", 9600, Family::Bridge);
    CHECK(id.kind == Kind::Gnss && id.chip == "MediaTek");
    CHECK_EQ(behind_text(id), std::string("GPS receiver"));
    for (int k = 0; k <= static_cast<int>(Kind::None); ++k) { /* a value beside its key on a 640x480 row */
        Identity t;
        t.kind = static_cast<Kind>(k);
        CHECK(behind_text(t).size() <= 16);
        t.chip = "a chip name that is far too long";
        CHECK(behind_text(t).size() <= 16);
    }

    id = run("marlin-boot.txt", 250000, Family::Arduino);
    CHECK(id.kind == Kind::Marlin && id.firmware == "Marlin" && id.version == "2.1.2.1" && id.baud == 250000);
    id = run("marlin-m115.txt", 115200, Family::Arduino);
    CHECK(id.kind == Kind::Marlin && id.version == "2.1.2.1" && id.board == "Ender-3 V2");

    id = run("grbl.txt", 115200, Family::Arduino);
    CHECK(id.kind == Kind::Grbl && id.firmware == "Grbl" && id.version == "1.1h");
    id = run("fluidnc.txt", 115200);
    CHECK(id.kind == Kind::Grbl && id.firmware == "FluidNC" && id.version == "v3.7.9");

    id = run("uboot.txt", 115200, Family::Bridge);
    CHECK(id.kind == Kind::UBoot && id.version == "2023.07" && id.board == "Raspberry Pi 4 Model B Rev 1.4");

    id = run("linux-boot.txt", 115200, Family::Bridge);
    CHECK(id.kind == Kind::Linux && id.version == "6.1.0-rpi7-rpi-v8");
    CHECK_EQ(id.detail, std::string("Debian GNU/Linux 12 (bookworm)"));

    id = run("openwrt.txt", 115200, Family::Bridge);
    CHECK(id.kind == Kind::OpenWrt && id.firmware == "OpenWrt" && id.version == "23.05.2");

    id = run("cisco-ios.txt", 9600, Family::Console);
    CHECK(id.kind == Kind::Cisco && id.board == "C2960" && id.version == "15.0(2)SE11");

    id = run("mikrotik.txt", 115200, Family::Bridge);
    CHECK(id.kind == Kind::MikroTik && id.firmware == "RouterOS" && id.version == "7.12");

    id = run("micropython.txt", 115200, Family::NativeUsb, true);
    CHECK(id.kind == Kind::MicroPython && id.version == "v1.22.2" && id.board == "Raspberry Pi Pico");
    CHECK_EQ(id.chip, std::string("RP2040"));
    CHECK_EQ(behind_text(id), std::string("RP2040"));

    id = run("circuitpython.txt", 115200, Family::NativeUsb, true);
    CHECK(id.kind == Kind::CircuitPython && id.version == "9.0.0" && id.chip == "RP2040");

    id = run("at-sim800.txt", 115200, Family::EspBridge);
    CHECK(id.kind == Kind::AtModem && id.chip == "SIM800" && id.version == "14.18");
    id = run("at-quectel.txt", 115200, Family::NativeUsb, true);
    CHECK(id.kind == Kind::AtModem && id.firmware == "Quectel" && id.version == "EC25EFAR06A06M4G");
    id = run("esp-at.txt", 115200);
    CHECK(id.kind == Kind::AtModem && id.firmware == "ESP-AT" && id.version == "2.2.0.0");

    id = run("plain-text.txt", 9600, Family::Bridge);
    CHECK(id.kind == Kind::Text && id.baud == 9600 && id.confidence < 40);
    id = run("plain-text.txt", 9600, Family::Arduino);
    CHECK(id.kind == Kind::Sketch);
    CHECK_EQ(std::string(confidence_word(id.confidence)), std::string("Likely"));
}

void test_speeds_and_noise()
{
    Context ctx;
    ctx.usb = Family::EspBridge;
    // nothing at all
    Identity id = classify({Capture{"", 115200}, Capture{"", 9600}}, ctx);
    CHECK(id.kind == Kind::Silent && !id.found() && id.baud == 0);
    CHECK_EQ(behind_text(id), std::string("Nothing heard"));
    CHECK(!identity_recommendation(id, nullptr, nullptr));
    // noise at 115200 and 9600, the ESP8266 ROM clean at 74880: found there
    id = classify({Capture{noise(300, 1), 115200}, Capture{noise(40, 2), 9600},
                   Capture{fixture("esp8266-rom-74880.txt"), 74880}}, ctx);
    CHECK(id.kind == Kind::Esp8266 && id.baud == 74880);
    // a reset heard at 115200 (garbled ROM start, then the sketch's text) and the ROM at 74880: the sketch's speed
    const std::string boot115 = noise(200, 3) + "\r\nHello from the ESP8266 sketch\r\nWiFi connected\r\n";
    id = classify({Capture{boot115, 115200}, Capture{fixture("esp8266-rom-74880.txt"), 74880}}, ctx);
    CHECK(id.kind == Kind::Esp8266 && id.baud == 115200);
    bool marker = false;
    for (const std::string &l : id.boot) marker = marker || l.find("at 74880") != std::string::npos;
    CHECK(marker); /* the boot output shows both */
    // noise only: binary or another speed, nothing recommended
    id = classify({Capture{noise(500, 4), 115200}}, ctx);
    CHECK(id.kind == Kind::Binary && id.confidence < 40 && !identity_recommendation(id, nullptr, nullptr));

    // a big capture (3 s of a busy log at 115200, about 35 kB) is classified quickly: the probes classify after each
    // speed, on a Pi Zero 2 W too
    std::string big;
    for (int i = 0; big.size() < 35000; ++i)
        big += "I (" + std::to_string(1000 + i) + ") app: line " + std::to_string(i) + " of a busy log, value 42\r\n";
    const std::int64_t t0 = now_ms_for_test();
    id = classify({Capture{big, 115200}}, ctx);
    CHECK(id.kind == Kind::EspIdf);
    CHECK(now_ms_for_test() - t0 < 300);
}

/* frames with their real checksums */
std::string ubx(std::uint8_t cls, std::uint8_t mid, const std::string &payload)
{
    std::string f = "\xB5\x62";
    f += static_cast<char>(cls);
    f += static_cast<char>(mid);
    f += static_cast<char>(payload.size() & 0xFF);
    f += static_cast<char>(payload.size() >> 8);
    f += payload;
    std::uint8_t a = 0, b = 0;
    for (std::size_t i = 2; i < f.size(); ++i) {
        a = static_cast<std::uint8_t>(a + static_cast<std::uint8_t>(f[i]));
        b = static_cast<std::uint8_t>(b + a);
    }
    f += static_cast<char>(a);
    f += static_cast<char>(b);
    return f;
}

std::string klipper(std::uint8_t seq, const std::string &payload)
{
    std::string f;
    f += static_cast<char>(payload.size() + 5);
    f += static_cast<char>(0x10 | (seq & 0x0F));
    f += payload;
    const std::uint16_t crc = crc16_klipper(f);
    f += static_cast<char>(crc >> 8);
    f += static_cast<char>(crc & 0xFF);
    f += '\x7E';
    return f;
}

std::string mavlink_heartbeat(std::uint8_t seq)
{
    std::string f = "\xFE\x09";
    f += static_cast<char>(seq);
    f += std::string("\x01\x01\x00", 3);             /* system 1, component 1, message 0 */
    f += std::string("\x00\x00\x00\x00\x02\x03\x51\x04\x03", 9); /* HEARTBEAT payload */
    std::uint16_t crc = crc_x25(f.substr(1));
    crc = crc_x25(std::string(1, static_cast<char>(50)), crc);
    f += static_cast<char>(crc & 0xFF);
    f += static_cast<char>(crc >> 8);
    return f;
}

std::string rtcm(const std::string &payload)
{
    std::string f = "\xD3";
    f += static_cast<char>((payload.size() >> 8) & 3);
    f += static_cast<char>(payload.size() & 0xFF);
    f += payload;
    const std::uint32_t crc = crc24q(f);
    f += static_cast<char>((crc >> 16) & 0xFF);
    f += static_cast<char>((crc >> 8) & 0xFF);
    f += static_cast<char>(crc & 0xFF);
    return f;
}

void test_binary()
{
    Context ctx;
    ctx.usb = Family::Bridge;
    int frames = 0;
    Kind kind = Kind::None;

    std::string u;
    for (int i = 0; i < 4; ++i) u += ubx(0x01, 0x07, std::string(92, static_cast<char>(i)));
    CHECK(binary_framing(u, &frames, &kind).compare(0, 3, "UBX") == 0 && frames == 4 && kind == Kind::Gnss);
    Identity id = classify({Capture{noise(200, 9), 9600}, Capture{u, 38400}}, ctx);
    CHECK(id.kind == Kind::Gnss && id.baud == 38400 && id.framing.compare(0, 3, "UBX") == 0);
    std::string broken = u;
    broken[20] ^= 0x55; /* one frame's checksum fails: three left */
    binary_framing(broken, &frames, &kind);
    CHECK_EQ(frames, 3);

    std::string r = rtcm(std::string("\x3E\xD0\x00\x03\x8A\x0E\xDE\xEF\x34\xB4\xBD\x62\xAC\x09\x41\x98\x6F\x33", 18));
    CHECK(binary_framing(r + r, &frames, &kind).compare(0, 6, "RTCM 3") == 0 && frames == 2 && kind == Kind::Gnss);

    std::string m;
    for (int i = 0; i < 3; ++i) m += mavlink_heartbeat(static_cast<std::uint8_t>(i));
    CHECK(binary_framing(m, &frames, &kind).compare(0, 7, "MAVLink") == 0 && frames == 3);
    id = classify({Capture{m, 57600}}, ctx);
    CHECK(id.kind == Kind::Binary && id.baud == 57600 && id.framing.compare(0, 7, "MAVLink") == 0);
    CHECK(identity_recommendation(id, nullptr, nullptr));

    std::string k;
    for (int i = 0; i < 5; ++i) k += klipper(static_cast<std::uint8_t>(i), "\x01\x02\x03");
    CHECK(binary_framing(k, &frames, &kind).compare(0, 7, "Klipper") == 0 && frames == 5 && kind == Kind::Klipper);
    id = classify({Capture{k, 250000}}, ctx);
    CHECK(id.kind == Kind::Klipper && id.baud == 250000);

    std::string mesh;
    for (int i = 0; i < 3; ++i) mesh += std::string("\x94\xC3\x00\x06\x08\x01\x12\x02\x08\x01", 10);
    CHECK(binary_framing(mesh, &frames, &kind).compare(0, 10, "Meshtastic") == 0 && kind == Kind::Meshtastic);

    std::string slip;
    for (int i = 0; i < 4; ++i) slip += std::string("\xC0\x01\x08\x04\x00\xDB\xDC\x00\x00\xC0", 10);
    CHECK(binary_framing(slip, &frames, &kind).compare(0, 4, "SLIP") == 0 && frames >= 2);

    std::string pb; /* fields 1 (varint), 2 (bytes), 3 (fixed32), repeated */
    for (int i = 0; i < 4; ++i) pb += std::string("\x08\x96\x01\x12\x03\x61\x62\x63\x1d\x00\x00\x80\x3f", 13);
    CHECK(binary_framing(pb, &frames, &kind).find("protobuf") != std::string::npos);

    CHECK(binary_framing(noise(600, 11), &frames, &kind).empty() && frames == 0); /* noise validates nothing */
}

void test_texts_and_file()
{
    Identity id = run("gnss-ublox-m8.txt", 9600, Family::Bridge);
    id.level = 1;
    id.probe = "listen";
    id.when = 1767225600;
    // the settings file keeps it, evidence and boot output included
    Config c;
    c.set_identity("usb-Prolific-if00", id);
    const std::string text = c.serialize();
    CHECK(text.find("id_kind=gnss\n") != std::string::npos && text.find("id_ev=") != std::string::npos);
    const Config back = Config::parse(text);
    const Identity b = back.identity("usb-Prolific-if00");
    CHECK(b.kind == Kind::Gnss && b.chip == id.chip && b.version == id.version && b.baud == 9600);
    CHECK(b.when == id.when && b.level == 1 && b.probe == "listen" && b.confidence == id.confidence);
    CHECK(b.evidence == id.evidence);
    CHECK(b.boot.size() == std::min<std::size_t>(id.boot.size(), 16));
    CHECK(back.settings("usb-Prolific-if00") == PortSettings()); /* the settings of the section stay the defaults */
    CHECK(back.identity("never").kind == Kind::None);
    // hostile values: no new line or section can come out of a device's text
    Identity evil;
    evil.kind = Kind::Text;
    evil.evidence.push_back("line\n[port x]\nbaud=1200");
    evil.board = "a\rb";
    Config e;
    e.set_identity("p", evil);
    const Config eb = Config::parse(e.serialize());
    CHECK(eb.ports.size() == 1 && eb.identity("p").evidence.size() == 1 && eb.settings("p").baud == 0);
    CHECK(eb.identity("p").evidence[0].find('\n') == std::string::npos);
    // bad values are ignored, unknown id_ keys too, and a section of id_ keys without a kind is no identity
    const Config bad = Config::parse("v=2\n[port q]\nid_conf=999\nid_baud=x\nid_when=-5\nid_zzz=1\n");
    CHECK(bad.identity("q").kind == Kind::None && bad.idents.empty());

    // texts
    CHECK_EQ(std::string(confidence_word(80)), std::string("Sure"));
    CHECK_EQ(std::string(confidence_word(45)), std::string("Likely"));
    CHECK_EQ(std::string(confidence_word(10)), std::string("A guess"));
    for (int k = 0; k <= static_cast<int>(Kind::None); ++k) {
        Identity t;
        t.kind = static_cast<Kind>(k);
        t.baud = 115200;
        t.chip = "ESP32-S3";
        CHECK(kind_from_key(kind_key(t.kind)) == t.kind);
        std::string reason;
        if (identity_recommendation(t, nullptr, &reason)) CHECK(!reason.empty() && reason.size() <= 48);
    }
    const std::string summary = identity_summary(id, "PL2303 USB-serial", "ttyUSB1");
    CHECK(summary.find("Behind it:   GPS / GNSS receiver") != std::string::npos);
    CHECK(summary.find("Recommended: 9600 8N1") != std::string::npos);

    // what is remembered: a new silence keeps an older finding; the ROM's chip stays under a later log
    Identity rom;
    rom.kind = Kind::EspRom;
    rom.chip = "ESP32-S3";
    rom.detail = "MAC f4:12:fa:aa:bb:cc";
    rom.confidence = 100;
    Identity silent;
    silent.kind = Kind::Silent;
    CHECK(remembered_identity(rom, silent).kind == Kind::EspRom);
    Identity log;
    log.kind = Kind::EspIdf;
    log.confidence = 35;
    const Identity kept = remembered_identity(rom, log);
    CHECK(kept.kind == Kind::EspIdf && kept.chip == "ESP32-S3" && kept.detail == rom.detail);
    Identity gps = run("gnss-mtk.txt", 9600, Family::Bridge);
    CHECK(remembered_identity(rom, gps).chip == "MediaTek"); /* another device: no mixing */

    // the recommendation replaces the table's
    Recommendation base = recommend(lookup_device(0x067b, 0x2303), false);
    const Recommendation r = identified_recommendation(base, id);
    CHECK(r.baud == 9600 && r.reason == "GPS found: NMEA at 9600" && r.why == "9600 8N1: GPS found: NMEA at 9600");
    CHECK(identified_recommendation(base, silent).baud == base.baud);
}

} // namespace

void test_identify()
{
    test_patterns();
    test_crcs();
    test_fixtures();
    test_speeds_and_noise();
    test_binary();
    test_texts_and_file();
}
