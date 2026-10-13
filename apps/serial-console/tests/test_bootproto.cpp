/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the boot loader protocols (core/bootproto: STK500 v1 / v2 framing, SLIP, the esptool packets and their
 * checksum, the chip and MAC tables, the read-only whitelists) and of the DTR / RTS sequences (core/linectl) on the
 * recording fake: their order, the ESP auto-reset circuit never booting into the download mode by mistake, and the
 * lines always put back.
 */
#include "check.hpp"

#include "bootproto.hpp"
#include "linectl.hpp"

using namespace sc;

namespace {

std::string bytes(std::initializer_list<int> list)
{
    std::string s;
    for (const int b : list) s += static_cast<char>(b);
    return s;
}

void test_stk500()
{
    CHECK_EQ(stk::frame(stk::kGetSync), bytes({0x30, 0x20}));
    CHECK_EQ(stk::frame(stk::kReadSign), bytes({0x75, 0x20}));
    CHECK_EQ(stk::frame(stk::kLeaveProgmode), bytes({0x51, 0x20}));
    CHECK_EQ(stk::frame(stk::kGetParameter, bytes({0x81})), bytes({0x41, 0x81, 0x20}));
    CHECK(stk::frame(stk::kGetParameter, bytes({0x98})).empty()); /* only the version parameters */
    CHECK(stk::frame(stk::kGetSync, "x").empty());
    // every command that writes, erases or is not needed is refused: no frame, nothing sent
    for (const int c : {0x40, 0x42, 0x45, 0x50, 0x52, 0x55, 0x56, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x74, 0x31})
        CHECK(!stk::allowed(static_cast<std::uint8_t>(c)) && stk::frame(static_cast<std::uint8_t>(c)).empty());
    std::string p;
    CHECK(stk::parse_reply(bytes({0x00, 0xFF, 0x14, 0x10}), 0, nullptr)); /* a stray byte before it */
    CHECK(stk::parse_reply(bytes({0x14, 0x1E, 0x95, 0x0F, 0x10}), 3, &p) && p == bytes({0x1E, 0x95, 0x0F}));
    CHECK(!stk::parse_reply(bytes({0x14, 0x1E, 0x95}), 3, nullptr));
    CHECK(!stk::parse_reply(bytes({0x15, 0x10}), 0, nullptr)); /* NOSYNC */
    CHECK_EQ(avr_chip_name(0x1E, 0x95, 0x0F), std::string("ATmega328P"));
    CHECK_EQ(avr_chip_name(0x1E, 0x98, 0x01), std::string("ATmega2560"));
    CHECK_EQ(avr_chip_name(0x1E, 0x95, 0x87), std::string("ATmega32U4"));
    CHECK(avr_chip_name(0x00, 0x00, 0x00).empty());

    // STK500 v2: start, sequence, size (big-endian), token, body, XOR checksum
    const std::string on = stk2::message(1, bytes({stk2::kSignOn}));
    CHECK_EQ(on, bytes({0x1B, 0x01, 0x00, 0x01, 0x0E, 0x01, 0x14}));
    CHECK(stk2::message(2, bytes({0x13, 0x00})).empty()); /* CMD_PROGRAM_FLASH_ISP */
    CHECK(stk2::message(2, bytes({0x12})).empty());       /* CMD_CHIP_ERASE_ISP */
    CHECK(stk2::message(2, bytes({0x10})).empty());       /* CMD_ENTER_PROGMODE_ISP: not needed */
    CHECK(stk2::message(2, std::string()).empty());
    std::string answer = bytes({0x1B, 0x01, 0x00, 0x0B, 0x0E, 0x01, 0x00, 0x08}) + "AVRISP_2";
    std::uint8_t sum = 0;
    for (const char c : answer) sum ^= static_cast<std::uint8_t>(c);
    answer += static_cast<char>(sum);
    std::uint8_t seq = 0;
    std::string body;
    CHECK(stk2::parse_message("junk" + answer, &seq, &body) && seq == 1 && body.substr(3) == "AVRISP_2");
    answer.back() ^= 1;
    CHECK(!stk2::parse_message(answer, nullptr, nullptr)); /* a bad checksum */
    CHECK_EQ(stk2::read_signature_body(1), bytes({0x1B, 0x04, 0x30, 0x00, 0x01, 0x00}));
}

void test_slip_esp()
{
    CHECK_EQ(slip::encode(bytes({0x01, 0xC0, 0xDB, 0x02})), bytes({0xC0, 0x01, 0xDB, 0xDC, 0xDB, 0xDD, 0x02, 0xC0}));
    slip::Decoder d;
    d.feed("ets Jun  8 2016\r\nwaiting for download\r\n"); /* text before the first frame is skipped */
    d.feed(bytes({0xC0, 0x01, 0xDB}));                   /* a frame split across reads */
    d.feed(bytes({0xDC, 0x02, 0xC0, 0xC0, 0x05, 0xC0}));
    std::string pk;
    CHECK(d.next(pk) && pk == bytes({0x01, 0xC0, 0x02}));
    CHECK(d.next(pk) && pk == bytes({0x05}));
    CHECK(!d.next(pk));
    d.feed(bytes({0xC0, 0x01, 0xDB, 0x77, 0x02, 0xC0})); /* a broken escape: the frame is dropped */
    CHECK(!d.next(pk));

    // the packets, byte for byte (esptool serial protocol)
    const std::string sync = esprom::sync_packet();
    CHECK_EQ(sync.size(), static_cast<std::size_t>(8 + 36));
    CHECK_EQ(sync.substr(0, 8), bytes({0x00, 0x08, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK_EQ(sync.substr(8, 4), bytes({0x07, 0x07, 0x12, 0x20}));
    CHECK_EQ(sync.substr(12), std::string(32, '\x55'));
    CHECK_EQ(esprom::read_reg_packet(0x40001000),
             bytes({0x00, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x40}));
    CHECK_EQ(esprom::security_info_packet(), bytes({0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK_EQ(esprom::checksum(bytes({0x01, 0x02})), static_cast<std::uint32_t>(0xEC));
    CHECK_EQ(esprom::checksum(std::string()), static_cast<std::uint32_t>(0xEF));
    // never a write or an erase: FLASH_BEGIN/DATA/END, MEM_*, WRITE_REG, SPI_ATTACH, ERASE_FLASH/REGION, CHANGE_BAUD...
    for (const int op : {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x10, 0x11, 0x12, 0x13, 0xD0, 0xD1, 0xD2})
        CHECK(!esprom::allowed(static_cast<std::uint8_t>(op)) && esprom::command(static_cast<std::uint8_t>(op), "x").empty());

    esprom::Response r;
    CHECK(esprom::parse_response(bytes({0x01, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}), r));
    CHECK(r.op == esprom::kSync && r.ok);
    CHECK(esprom::parse_response(bytes({0x01, 0x0A, 0x02, 0x00, 0x83, 0x1D, 0xF0, 0x00, 0x00, 0x00}), r)); /* ESP8266-style */
    CHECK(r.op == esprom::kReadReg && r.ok && r.value == 0x00F01D83u);
    CHECK(esprom::parse_response(bytes({0x01, 0x14, 0x04, 0x00, 0, 0, 0, 0, 0x01, 0x05, 0x00, 0x00}), r));
    CHECK(!r.ok && r.error == 0x05); /* not supported by the ESP32's ROM */
    CHECK(!esprom::security_chip_id(r, nullptr));
    std::string sec = bytes({0x01, 0x14, 0x18, 0x00, 0, 0, 0, 0});
    sec += std::string(12, '\0') + bytes({0x09, 0, 0, 0}) + bytes({0, 0, 0, 0}) + bytes({0, 0, 0, 0});
    std::uint32_t chip_id = 0;
    CHECK(esprom::parse_response(sec, r) && r.ok && esprom::security_chip_id(r, &chip_id) && chip_id == 9);
    CHECK(!esprom::parse_response(bytes({0x00, 0x08, 0x04, 0x00, 0, 0, 0, 0}), r)); /* a request, not a response */

    CHECK_EQ(esprom::chip_from_magic(0xFFF0C101u), std::string("ESP8266"));
    CHECK_EQ(esprom::chip_from_magic(0x00F01D83u), std::string("ESP32"));
    CHECK_EQ(esprom::chip_from_magic(0x000007C6u), std::string("ESP32-S2"));
    CHECK_EQ(esprom::chip_from_magic(0x00000009u), std::string("ESP32-S3"));
    CHECK_EQ(esprom::chip_from_magic(0x1B31506Fu), std::string("ESP32-C3"));
    CHECK_EQ(esprom::chip_from_magic(0x2CE0806Fu), std::string("ESP32-C6"));
    CHECK_EQ(esprom::chip_from_magic(0xD7B73E80u), std::string("ESP32-H2"));
    CHECK(esprom::chip_from_magic(0x12345678u).empty());
    CHECK_EQ(esprom::chip_from_id(9), std::string("ESP32-S3"));
    CHECK_EQ(esprom::chip_from_id(13), std::string("ESP32-C6"));
    CHECK_EQ(esprom::chip_from_id(23), std::string("ESP32-C5"));
    CHECK(esprom::chip_from_id(99).empty());

    CHECK(esprom::mac_registers("ESP32") == std::vector<std::uint32_t>({0x3FF5A004u, 0x3FF5A008u}));
    CHECK(esprom::mac_registers("ESP32-S3") == std::vector<std::uint32_t>({0x60007044u, 0x60007048u}));
    CHECK_EQ(esprom::mac_registers("ESP8266").size(), static_cast<std::size_t>(3));
    CHECK(esprom::mac_registers("ESP32-P4").empty());
    CHECK_EQ(esprom::mac_from_registers("ESP32", {0xC4123456u, 0x6B00240Au}), std::string("24:0a:c4:12:34:56"));
    CHECK_EQ(esprom::mac_from_registers("ESP32-S3", {0xFAAABBCCu, 0x0000F412u}), std::string("f4:12:fa:aa:bb:cc"));
    CHECK_EQ(esprom::mac_from_registers("ESP8266", {0x12000000u, 0x0000ABCDu, 0}), std::string("18:fe:34:ab:cd:12"));
    CHECK_EQ(esprom::mac_from_registers("ESP8266", {0x12000000u, 0x0001ABCDu, 0}), std::string("ac:d0:74:ab:cd:12"));
    CHECK(esprom::mac_from_registers("ESP32", {1}).empty());
}

/* The ESP auto-reset circuit (two transistors): EN low when RTS is asserted and DTR is not; IO0 low when DTR is
 * asserted and RTS is not. Plays the steps from `start`; counts the boots into the program and into the ROM's
 * download mode (EN rising with IO0 high / low). */
void esp_boots(const std::vector<LineStep> &steps, Lines start, int *run, int *download)
{
    *run = *download = 0;
    bool en_low = start.rts && !start.dtr;
    for (const LineStep &s : steps) {
        if (s.kind != LineStep::Set) continue;
        const bool low = s.lines.rts && !s.lines.dtr;
        if (en_low && !low) {
            if (s.lines.dtr && !s.lines.rts) ++*download;
            else ++*run;
        }
        en_low = low;
    }
}

/* An Arduino resets on each DTR assertion (the capacitor to RESET). */
int arduino_resets(const std::vector<LineStep> &steps, Lines start)
{
    int n = 0;
    bool dtr = start.dtr;
    for (const LineStep &s : steps) {
        if (s.kind != LineStep::Set) continue;
        if (s.lines.dtr && !dtr) ++n;
        dtr = s.lines.dtr;
    }
    return n;
}

void test_lines()
{
    const Lines open{true, true}; /* as Linux leaves them after open() */
    const std::vector<LineStep> reset = run_reset_steps(open, ResetStyle::AutoReset);
    CHECK_EQ(steps_text(reset), std::string("D0R1 w100 D0R0 w150 D1R1 w50"));
    int run = 0, dl = 0;
    esp_boots(reset, open, &run, &dl);
    CHECK(run == 1 && dl == 0); /* level 2 boots an ESP into its program, never into the download mode */
    CHECK_EQ(arduino_resets(reset, open), 1); /* and restarts an Arduino once */
    CHECK_EQ(steps_text(run_reset_steps(Lines{false, false}, ResetStyle::AutoReset)),
             std::string("D0R1 w100 D0R0 w150 D1R1 w50 D0R0")); /* back to where they were */
    CHECK(run_reset_steps(open, ResetStyle::None).empty());
    CHECK_EQ(steps_text(run_reset_steps(open, ResetStyle::UsbJtag)), steps_text(reset));

    const std::vector<LineStep> boot = esp_bootloader_steps(ResetStyle::AutoReset);
    CHECK_EQ(steps_text(boot), std::string("D0R0 D1R1 D0R1 w100 D1R0 w50 D0R0"));
    esp_boots(boot, open, &run, &dl);
    CHECK(dl == 1 && run == 0);
    CHECK_EQ(steps_text(esp_bootloader_steps(ResetStyle::AutoReset, 550)),
             std::string("D0R0 D1R1 D0R1 w100 D1R0 w550 D0R0"));
    CHECK_EQ(steps_text(esp_bootloader_steps(ResetStyle::UsbJtag)),
             std::string("D0R0 w100 D1R0 w100 D1R1 D0R1 w100 D0R0"));
    const std::vector<LineStep> hard = esp_hard_reset_steps(ResetStyle::AutoReset);
    CHECK_EQ(steps_text(hard), std::string("D0R1 w100 D0R0"));
    esp_boots(hard, Lines{false, false}, &run, &dl);
    CHECK(run == 1 && dl == 0); /* out of the ROM into the program */
    CHECK_EQ(steps_text(esp_hard_reset_steps(ResetStyle::UsbJtag)), std::string("D0R1 w200 D0R0 w200"));
    CHECK_EQ(steps_text(arduino_reset_steps()), std::string("D0R0 w250 D1R1 w50"));
    CHECK_EQ(arduino_resets(arduino_reset_steps(), open), 1);

    // played on the recording fake: the order of the changes and the waits, then the guard puts them back
    {
        FakeLines f(open);
        std::vector<Lines> seen;
        f.set_listener([&seen](const Lines &l) { seen.push_back(l); });
        {
            LineGuard guard(f);
            CHECK(guard.supported() && guard.original() == open);
            int waited = 0;
            CHECK(run_steps(f, esp_bootloader_steps(ResetStyle::AutoReset), [&waited](int ms) {
                waited += ms;
                return true;
            }));
            CHECK_EQ(waited, 150);
            CHECK(f.state() == (Lines{false, false}));
        }
        CHECK(f.state() == open); /* restored */
        CHECK_EQ(f.trace(), std::string("D0R0 D1R1 D0R1 w100 D1R0 w50 D0R0 D1R1"));
        CHECK_EQ(seen.size(), f.history().size());
    }
    // cancelled in the middle (EN held low): the guard still restores, so no board stays in reset
    {
        FakeLines f(open);
        {
            LineGuard guard(f);
            CHECK(!run_steps(f, run_reset_steps(open, ResetStyle::AutoReset), [](int) { return false; }));
            CHECK(f.state() == (Lines{false, true})); /* stopped with RTS asserted: EN low */
        }
        CHECK(f.state() == open);
        CHECK_EQ(f.trace(), std::string("D0R1 w100 D1R1"));
    }
    // nothing changed: nothing is set back (no extra DTR edge for an Arduino)
    {
        FakeLines f(open);
        { LineGuard guard(f); }
        CHECK(f.history().empty());
    }
    // a port without modem lines: nothing is set, the guard knows
    {
        FakeLines f(open, false);
        LineGuard guard(f);
        CHECK(!guard.supported());
        CHECK(!run_steps(f, run_reset_steps(open, ResetStyle::AutoReset), [](int) { return true; }));
        CHECK(f.history().empty());
    }
}

} // namespace

void test_bootproto()
{
    test_stk500();
    test_slip_esp();
    test_lines();
}
