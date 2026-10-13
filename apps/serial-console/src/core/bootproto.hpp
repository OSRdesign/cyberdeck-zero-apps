/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the two boot loader protocols the "Ask" probes speak, byte for byte, READ-ONLY (no LVGL, no I/O;
 * tests/test_bootproto.cpp). Every frame the app can build passes a whitelist of commands that only read: a builder
 * asked for anything else (a flash write, an erase, a register write) returns an empty frame, and the tests check it.
 *
 * STK500 version 1 (Arduino Uno / Nano / Pro Mini boot loaders: optiboot at 115200, the older ATmegaBOOT at 57600):
 *   GET_SYNC 0x30 0x20 -> 0x14 0x10 (in sync, OK); READ_SIGN 0x75 0x20 -> 0x14 s1 s2 s3 0x10 (the chip's signature,
 *   1E 95 0F = ATmega328P); GET_PARAMETER 0x41 p 0x20 -> 0x14 v 0x10 (0x81 / 0x82: the boot loader's version);
 *   LEAVE_PROGMODE 0x51 0x20 -> 0x14 0x10 (optiboot then starts the sketch).
 * STK500 version 2 (the Arduino Mega 2560's boot loader, 115200): a message is 0x1B, a sequence number, the body size
 *   (2 bytes, big-endian), 0x0E, the body, and an XOR checksum of all of it. CMD_SIGN_ON 0x01 -> "AVRISP_2";
 *   CMD_READ_SIGNATURE_ISP 0x1B (one byte per call); CMD_LEAVE_PROGMODE_ISP 0x11.
 *
 * Espressif ROM boot loader ("esptool" serial protocol, every ESP8266 / ESP32 chip): SLIP frames (0xC0 ends, 0xC0 and
 *   0xDB escaped as DB DC / DB DD) around a packet: direction 0x00, command, data size (2 bytes LE), checksum (4 bytes
 *   LE, only for data commands: 0xEF xor the data bytes), data. The answer: direction 0x01, the command, size, a 32-bit
 *   value (READ_REG's result), data ending with the status bytes (0 = success). Read-only commands used: SYNC 0x08
 *   (07 07 12 20 then 32 x 0x55), READ_REG 0x0A (an address), GET_SECURITY_INFO 0x14 (the chip id of the newer chips).
 *   The chip is named as esptool names it: the chip id of GET_SECURITY_INFO when the ROM has one, else the "magic"
 *   value at 0x40001000. Sources: esptool's documentation (serial protocol) and its chip classes.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

/* ---------------------------------------------------------------- STK500 v1 */

namespace stk {
constexpr std::uint8_t kInSync = 0x14;
constexpr std::uint8_t kOk = 0x10;
constexpr std::uint8_t kCrcEop = 0x20;
constexpr std::uint8_t kGetSync = 0x30;
constexpr std::uint8_t kGetParameter = 0x41;
constexpr std::uint8_t kLeaveProgmode = 0x51;
constexpr std::uint8_t kReadSign = 0x75;
constexpr std::uint8_t kSwMajor = 0x81;
constexpr std::uint8_t kSwMinor = 0x82;

/* The read-only commands; anything else (0x50 enter progmode, 0x55 load address, 0x60-0x64 program, 0x74 read page
 * included: it is not needed) is refused. */
bool allowed(std::uint8_t command);
/* command + args + 0x20; "" when the command is not allowed */
std::string frame(std::uint8_t command, const std::string &args = std::string());
/* An answer "0x14 <n bytes> 0x10" found in what came (a stray byte before it is skipped); payload gets the n bytes. */
bool parse_reply(const std::string &bytes, std::size_t n, std::string *payload);
} // namespace stk

/* ---------------------------------------------------------------- STK500 v2 */

namespace stk2 {
constexpr std::uint8_t kStart = 0x1B;
constexpr std::uint8_t kToken = 0x0E;
constexpr std::uint8_t kSignOn = 0x01;
constexpr std::uint8_t kLeaveProgmodeIsp = 0x11;
constexpr std::uint8_t kReadSignatureIsp = 0x1B;
constexpr std::uint8_t kStatusOk = 0x00;

bool allowed(std::uint8_t command);
/* the whole message; "" when body[0] is not an allowed command */
std::string message(std::uint8_t seq, const std::string &body);
/* A valid message (start, size, token, checksum) in what came: its sequence number and body. */
bool parse_message(const std::string &bytes, std::uint8_t *seq, std::string *body);
std::string read_signature_body(int index); /* index 0..2 */
} // namespace stk2

/* "ATmega328P" for 1E 95 0F, "" when unknown */
std::string avr_chip_name(std::uint8_t s1, std::uint8_t s2, std::uint8_t s3);

/* ---------------------------------------------------------------- SLIP and the ESP ROM protocol */

namespace slip {
constexpr std::uint8_t kEnd = 0xC0;
constexpr std::uint8_t kEsc = 0xDB;
constexpr std::uint8_t kEscEnd = 0xDC;
constexpr std::uint8_t kEscEsc = 0xDD;

std::string encode(const std::string &packet); /* C0 ... C0 */

/* A stream decoder: feed bytes, take whole packets. */
class Decoder {
public:
    void feed(const std::string &bytes);
    bool next(std::string &packet);
    void clear();

private:
    std::string cur_;
    bool in_frame_ = false, esc_ = false;
    std::vector<std::string> done_;
};
} // namespace slip

namespace esprom {
constexpr std::uint8_t kSync = 0x08;
constexpr std::uint8_t kReadReg = 0x0A;
constexpr std::uint8_t kGetSecurityInfo = 0x14;
constexpr std::uint32_t kChipDetectMagicReg = 0x40001000;

/* SYNC, READ_REG and GET_SECURITY_INFO only; FLASH_BEGIN 0x02, FLASH_DATA 0x03, WRITE_REG 0x09, ERASE 0xD0 ... are
 * refused. */
bool allowed(std::uint8_t command);
std::uint32_t checksum(const std::string &data); /* 0xEF xor every byte */
/* The packet (not SLIP-encoded yet); "" when the command is not allowed. */
std::string command(std::uint8_t op, const std::string &data, std::uint32_t chk = 0);
std::string sync_packet();
std::string read_reg_packet(std::uint32_t address);
std::string security_info_packet();

struct Response {
    std::uint8_t op = 0;
    std::uint32_t value = 0;
    std::string data;   /* without the status bytes */
    bool ok = false;    /* the status byte was 0 */
    std::uint8_t error = 0;
};
bool parse_response(const std::string &packet, Response &out);

/* The chip from the magic value at 0x40001000 ("ESP32-S3"; "" unknown) and from GET_SECURITY_INFO's chip id. */
std::string chip_from_magic(std::uint32_t magic);
std::string chip_from_id(std::uint32_t chip_id);
/* GET_SECURITY_INFO's chip id (only the newer ROMs give one: 20 data bytes); false when absent. */
bool security_chip_id(const Response &r, std::uint32_t *chip_id);

/* The MAC address: the eFuse registers to read for this chip, and the address from their values ("" when the chip's
 * eFuse layout is not known here). Addresses and layouts are esptool's. */
std::vector<std::uint32_t> mac_registers(const std::string &chip);
std::string mac_from_registers(const std::string &chip, const std::vector<std::uint32_t> &values);
} // namespace esprom

} // namespace sc
