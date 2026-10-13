/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the read-only boot loader protocols (see bootproto.hpp).
 */
#include "bootproto.hpp"

#include <cstdio>

namespace sc {
namespace {

std::string le16(std::uint32_t v)
{
    std::string s;
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
    return s;
}

std::string le32(std::uint32_t v)
{
    std::string s = le16(v & 0xFFFF);
    s += le16(v >> 16);
    return s;
}

std::uint32_t get_le16(const std::string &s, std::size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8);
}

std::uint32_t get_le32(const std::string &s, std::size_t at)
{
    return get_le16(s, at) | (get_le16(s, at + 2) << 16);
}

std::uint8_t byte_at(const std::string &s, std::size_t i)
{
    return static_cast<std::uint8_t>(s[i]);
}

} // namespace

/* ---------------------------------------------------------------- STK500 v1 */

namespace stk {

bool allowed(std::uint8_t command)
{
    return command == kGetSync || command == kGetParameter || command == kReadSign || command == kLeaveProgmode;
}

std::string frame(std::uint8_t command, const std::string &args)
{
    if (!allowed(command)) return std::string();
    if (command == kGetParameter && (args.size() != 1 || (byte_at(args, 0) != kSwMajor && byte_at(args, 0) != kSwMinor &&
                                                          byte_at(args, 0) != 0x80)))
        return std::string(); /* only the version parameters */
    if (command != kGetParameter && !args.empty()) return std::string();
    std::string f(1, static_cast<char>(command));
    f += args;
    f += static_cast<char>(kCrcEop);
    return f;
}

bool parse_reply(const std::string &bytes, std::size_t n, std::string *payload)
{
    for (std::size_t i = 0; i + n + 2 <= bytes.size(); ++i) {
        if (byte_at(bytes, i) != kInSync || byte_at(bytes, i + n + 1) != kOk) continue;
        if (payload) *payload = bytes.substr(i + 1, n);
        return true;
    }
    return false;
}

} // namespace stk

/* ---------------------------------------------------------------- STK500 v2 */

namespace stk2 {

bool allowed(std::uint8_t command)
{
    return command == kSignOn || command == kReadSignatureIsp || command == kLeaveProgmodeIsp;
}

std::string message(std::uint8_t seq, const std::string &body)
{
    if (body.empty() || !allowed(byte_at(body, 0))) return std::string();
    std::string m;
    m += static_cast<char>(kStart);
    m += static_cast<char>(seq);
    m += static_cast<char>((body.size() >> 8) & 0xFF);
    m += static_cast<char>(body.size() & 0xFF);
    m += static_cast<char>(kToken);
    m += body;
    std::uint8_t sum = 0;
    for (const char c : m) sum ^= static_cast<std::uint8_t>(c);
    m += static_cast<char>(sum);
    return m;
}

bool parse_message(const std::string &bytes, std::uint8_t *seq, std::string *body)
{
    for (std::size_t i = 0; i + 6 <= bytes.size(); ++i) {
        if (byte_at(bytes, i) != kStart || byte_at(bytes, i + 4) != kToken) continue;
        const std::size_t size = (static_cast<std::size_t>(byte_at(bytes, i + 2)) << 8) | byte_at(bytes, i + 3);
        if (size == 0 || size > 275 || i + 5 + size + 1 > bytes.size()) continue;
        std::uint8_t sum = 0;
        for (std::size_t k = i; k < i + 5 + size; ++k) sum ^= byte_at(bytes, k);
        if (sum != byte_at(bytes, i + 5 + size)) continue;
        if (seq) *seq = byte_at(bytes, i + 1);
        if (body) *body = bytes.substr(i + 5, size);
        return true;
    }
    return false;
}

std::string read_signature_body(int index)
{
    /* CMD_READ_SIGNATURE_ISP, retAddr 4, then the ISP command bytes 0x30 0x00 <index> 0x00 */
    std::string b;
    b += static_cast<char>(kReadSignatureIsp);
    b += static_cast<char>(4);
    b += static_cast<char>(0x30);
    b += static_cast<char>(0x00);
    b += static_cast<char>(index & 3);
    b += static_cast<char>(0x00);
    return b;
}

} // namespace stk2

std::string avr_chip_name(std::uint8_t s1, std::uint8_t s2, std::uint8_t s3)
{
    static const struct {
        std::uint8_t a, b, c;
        const char *name;
    } kChips[] = {
        {0x1E, 0x95, 0x0F, "ATmega328P"}, {0x1E, 0x95, 0x14, "ATmega328"},   {0x1E, 0x95, 0x16, "ATmega328PB"},
        {0x1E, 0x94, 0x06, "ATmega168"},  {0x1E, 0x94, 0x0B, "ATmega168P"},  {0x1E, 0x93, 0x07, "ATmega8"},
        {0x1E, 0x98, 0x01, "ATmega2560"}, {0x1E, 0x97, 0x03, "ATmega1280"},  {0x1E, 0x97, 0x05, "ATmega1284P"},
        {0x1E, 0x96, 0x0A, "ATmega644P"}, {0x1E, 0x95, 0x87, "ATmega32U4"},  {0x1E, 0x93, 0x0F, "ATmega88P"},
    };
    for (const auto &c : kChips)
        if (c.a == s1 && c.b == s2 && c.c == s3) return c.name;
    return std::string();
}

/* ---------------------------------------------------------------- SLIP */

namespace slip {

std::string encode(const std::string &packet)
{
    std::string out(1, static_cast<char>(kEnd));
    for (const char ch : packet) {
        const std::uint8_t c = static_cast<std::uint8_t>(ch);
        if (c == kEnd) {
            out += static_cast<char>(kEsc);
            out += static_cast<char>(kEscEnd);
        } else if (c == kEsc) {
            out += static_cast<char>(kEsc);
            out += static_cast<char>(kEscEsc);
        } else {
            out += ch;
        }
    }
    out += static_cast<char>(kEnd);
    return out;
}

void Decoder::feed(const std::string &bytes)
{
    for (const char ch : bytes) {
        const std::uint8_t c = static_cast<std::uint8_t>(ch);
        if (c == kEnd) {
            if (in_frame_ && !cur_.empty()) done_.push_back(cur_);
            cur_.clear();
            in_frame_ = true; /* an end byte also starts the next frame */
            esc_ = false;
            continue;
        }
        if (!in_frame_) continue; /* text before the first frame (a boot banner) */
        if (esc_) {
            esc_ = false;
            if (c == kEscEnd) cur_ += static_cast<char>(kEnd);
            else if (c == kEscEsc) cur_ += static_cast<char>(kEsc);
            else { /* a broken escape: drop this frame */
                cur_.clear();
                in_frame_ = false;
            }
            continue;
        }
        if (c == kEsc) {
            esc_ = true;
            continue;
        }
        cur_ += ch;
        if (cur_.size() > 4096) { /* not a frame of this protocol */
            cur_.clear();
            in_frame_ = false;
        }
    }
}

bool Decoder::next(std::string &packet)
{
    if (done_.empty()) return false;
    packet = done_.front();
    done_.erase(done_.begin());
    return true;
}

void Decoder::clear()
{
    cur_.clear();
    done_.clear();
    in_frame_ = esc_ = false;
}

} // namespace slip

/* ---------------------------------------------------------------- the ESP ROM boot loader */

namespace esprom {

bool allowed(std::uint8_t command)
{
    return command == kSync || command == kReadReg || command == kGetSecurityInfo;
}

std::uint32_t checksum(const std::string &data)
{
    std::uint32_t c = 0xEF;
    for (const char ch : data) c ^= static_cast<std::uint8_t>(ch);
    return c;
}

std::string command(std::uint8_t op, const std::string &data, std::uint32_t chk)
{
    if (!allowed(op)) return std::string();
    std::string p;
    p += static_cast<char>(0x00);
    p += static_cast<char>(op);
    p += le16(static_cast<std::uint32_t>(data.size()));
    p += le32(chk);
    p += data;
    return p;
}

std::string sync_packet()
{
    std::string data = "\x07\x07\x12\x20";
    data += std::string(32, '\x55');
    return command(kSync, data);
}

std::string read_reg_packet(std::uint32_t address)
{
    return command(kReadReg, le32(address));
}

std::string security_info_packet()
{
    return command(kGetSecurityInfo, std::string());
}

bool parse_response(const std::string &packet, Response &out)
{
    if (packet.size() < 8 || byte_at(packet, 0) != 0x01) return false;
    const std::size_t size = get_le16(packet, 2);
    if (packet.size() < 8 + size || size < 2) return false;
    out.op = byte_at(packet, 1);
    out.value = get_le32(packet, 4);
    const std::string data = packet.substr(8, size);
    /* the status bytes end the data: 2 on the ESP8266 and the stub, 4 on the ESP32 ROMs; the first is 0 for success */
    const std::size_t status_len = (out.op == kGetSecurityInfo) ? (data.size() % 4 == 2 ? 2 : 4)
                                                                : (data.size() >= 4 ? 4 : 2);
    const std::size_t cut = data.size() >= status_len ? data.size() - status_len : 0;
    out.ok = byte_at(data, cut) == 0;
    out.error = cut + 1 < data.size() ? byte_at(data, cut + 1) : 0;
    out.data = data.substr(0, cut);
    return true;
}

std::string chip_from_magic(std::uint32_t magic)
{
    static const struct {
        std::uint32_t magic;
        const char *chip;
    } kMagic[] = {
        {0xFFF0C101u, "ESP8266"},  {0x00F01D83u, "ESP32"},    {0x000007C6u, "ESP32-S2"}, {0x00000009u, "ESP32-S3"},
        {0x6921506Fu, "ESP32-C3"}, {0x1B31506Fu, "ESP32-C3"}, {0x4881606Fu, "ESP32-C3"}, {0x4361606Fu, "ESP32-C3"},
        {0x6F51306Fu, "ESP32-C2"}, {0x7C41A06Fu, "ESP32-C2"}, {0x2CE0806Fu, "ESP32-C6"}, {0xD7B73E80u, "ESP32-H2"},
    };
    for (const auto &m : kMagic)
        if (m.magic == magic) return m.chip;
    return std::string();
}

std::string chip_from_id(std::uint32_t chip_id)
{
    switch (chip_id) {
    case 0: return "ESP32";
    case 2: return "ESP32-S2";
    case 5: return "ESP32-C3";
    case 9: return "ESP32-S3";
    case 12: return "ESP32-C2";
    case 13: return "ESP32-C6";
    case 16: return "ESP32-H2";
    case 18: return "ESP32-P4";
    case 20: return "ESP32-C61";
    case 23: return "ESP32-C5";
    default: return std::string();
    }
}

bool security_chip_id(const Response &r, std::uint32_t *chip_id)
{
    /* flags (4), flash_crypt_cnt (1), key_purposes (7), chip_id (4), api_version (4) */
    if (r.op != kGetSecurityInfo || !r.ok || r.data.size() < 20) return false;
    if (chip_id) *chip_id = get_le32(r.data, 12);
    return true;
}

namespace {
std::uint32_t efuse_mac_base(const std::string &chip)
{
    if (chip == "ESP32-S2") return 0x3F41A000u + 0x044;
    if (chip == "ESP32-S3") return 0x60007000u + 0x044;
    if (chip == "ESP32-C3") return 0x60008800u + 0x044;
    if (chip == "ESP32-C2") return 0x60008800u + 0x040;
    if (chip == "ESP32-C6" || chip == "ESP32-H2") return 0x600B0800u + 0x044;
    return 0;
}

std::string mac_text(const std::uint8_t (&b)[6])
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return buf;
}
} // namespace

std::vector<std::uint32_t> mac_registers(const std::string &chip)
{
    if (chip == "ESP8266") return {0x3FF00050u, 0x3FF00054u, 0x3FF0005Cu};
    if (chip == "ESP32") return {0x3FF5A004u, 0x3FF5A008u}; /* eFuse read words 1 and 2 */
    const std::uint32_t base = efuse_mac_base(chip);
    if (base) return {base, base + 4};
    return {};
}

std::string mac_from_registers(const std::string &chip, const std::vector<std::uint32_t> &v)
{
    std::uint8_t b[6] = {};
    if (chip == "ESP8266") {
        if (v.size() != 3) return std::string();
        const std::uint32_t mac0 = v[0], mac1 = v[1], mac3 = v[2];
        if (mac3 != 0) {
            b[0] = (mac3 >> 16) & 0xFF;
            b[1] = (mac3 >> 8) & 0xFF;
            b[2] = mac3 & 0xFF;
        } else if (((mac1 >> 16) & 0xFF) == 0) {
            b[0] = 0x18, b[1] = 0xFE, b[2] = 0x34;
        } else if (((mac1 >> 16) & 0xFF) == 1) {
            b[0] = 0xAC, b[1] = 0xD0, b[2] = 0x74;
        } else {
            return std::string();
        }
        b[3] = (mac1 >> 8) & 0xFF;
        b[4] = mac1 & 0xFF;
        b[5] = (mac0 >> 24) & 0xFF;
        return mac_text(b);
    }
    if (v.size() != 2 || mac_registers(chip).empty()) return std::string();
    /* low word first, then the high word: the MAC is the high word's low 16 bits, then the low word, big-endian */
    const std::uint32_t lo = v[0], hi = v[1];
    b[0] = (hi >> 8) & 0xFF;
    b[1] = hi & 0xFF;
    b[2] = (lo >> 24) & 0xFF;
    b[3] = (lo >> 16) & 0xFF;
    b[4] = (lo >> 8) & 0xFF;
    b[5] = lo & 0xFF;
    return mac_text(b);
}

} // namespace esprom
} // namespace sc
