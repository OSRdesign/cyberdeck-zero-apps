/*
 * SPDX-License-Identifier: MIT
 *
 * MeshCore companion protocol: command builders and response / push decoders.
 *
 * Pure C++ (no LVGL, no I/O). Written from meshcore_py (MIT; packets.py, reader.py, commands_*.py) and from
 * MeshCore's docs/companion_protocol.md (MIT). Where the two disagree this follows meshcore_py, because that is
 * a client known to work against the firmware. Differences are marked "doc differs" below and listed in the
 * hand-off report.
 */

#pragma once

#include "util.hpp"

#include <optional>
#include <string>
#include <variant>

namespace meshzero {

/* Command codes (first byte of an app -> device frame). */
namespace cmd {
constexpr uint8_t kAppStart = 1;
constexpr uint8_t kSendTxt = 2;
constexpr uint8_t kSendChannelTxt = 3;
constexpr uint8_t kGetContacts = 4;
constexpr uint8_t kGetTime = 5;
constexpr uint8_t kSetTime = 6;
constexpr uint8_t kSendAdvert = 7;
constexpr uint8_t kSetName = 8;
constexpr uint8_t kSyncNext = 10;
constexpr uint8_t kGetCustomVars = 40;
constexpr uint8_t kSetCustomVar = 41;
constexpr uint8_t kSetRadio = 11;
constexpr uint8_t kSetTxPower = 12;
constexpr uint8_t kResetPath = 13;
constexpr uint8_t kBattery = 20;
constexpr uint8_t kDeviceQuery = 22;
constexpr uint8_t kGetContactByKey = 30;
constexpr uint8_t kGetChannel = 31;
constexpr uint8_t kSetChannel = 32;
} // namespace cmd

/* Response and push codes (first byte of a device -> app frame). */
namespace resp {
constexpr uint8_t kOk = 0;
constexpr uint8_t kError = 1;
constexpr uint8_t kContactStart = 2;
constexpr uint8_t kContact = 3;
constexpr uint8_t kContactEnd = 4;
constexpr uint8_t kSelfInfo = 5;
constexpr uint8_t kMsgSent = 6;
constexpr uint8_t kContactMsg = 7;
constexpr uint8_t kChannelMsg = 8;
constexpr uint8_t kCurrentTime = 9;
constexpr uint8_t kNoMoreMsgs = 10;
constexpr uint8_t kBattery = 12;
constexpr uint8_t kDeviceInfo = 13;
constexpr uint8_t kContactMsgV3 = 16;
constexpr uint8_t kChannelMsgV3 = 17;
constexpr uint8_t kChannelInfo = 18;
constexpr uint8_t kCustomVars = 21;
constexpr uint8_t kChannelData = 27;
// pushes
constexpr uint8_t kAdvertisement = 0x80;
constexpr uint8_t kPathUpdate = 0x81;
constexpr uint8_t kAck = 0x82;
constexpr uint8_t kMessagesWaiting = 0x83;
constexpr uint8_t kLogData = 0x88;
constexpr uint8_t kNewAdvert = 0x8A;
constexpr uint8_t kContactDeleted = 0x8F;
constexpr uint8_t kContactsFull = 0x90;
} // namespace resp

/* ERR_CODE_* values of a kError response (byte 1). */
namespace err {
constexpr uint8_t kUnsupported = 1;
constexpr uint8_t kNotFound = 2;
constexpr uint8_t kTableFull = 3;
constexpr uint8_t kBadState = 4;
constexpr uint8_t kFileIo = 5;
constexpr uint8_t kIllegalArg = 6;
} // namespace err
std::string error_text(int code);

/* Contact / advert types. */
namespace advtype {
constexpr uint8_t kNone = 0;
constexpr uint8_t kChat = 1;
constexpr uint8_t kRepeater = 2;
constexpr uint8_t kRoom = 3;
constexpr uint8_t kSensor = 4;
} // namespace advtype
const char *contact_type_name(int type);

/* Text types of a message. */
namespace txttype {
constexpr uint8_t kPlain = 0;
constexpr uint8_t kCliData = 1;
constexpr uint8_t kSignedPlain = 2;
} // namespace txttype

/* Path length byte of contacts and messages: 0xFF means flood / no known path; otherwise the low 6 bits are the
 * hop count and the top 2 bits the hash size minus one (path_hash_mode). */
constexpr uint8_t kPathFlood = 0xFF;
int path_hops(uint8_t path_len);            // -1 for flood
int path_hash_mode(uint8_t path_len);       // -1 for flood

/* ------------------------------------------------------------------ decoded packets */

struct Ok {
    bool has_value = false;
    uint32_t value = 0;
};
struct Error {
    int code = -1;                          // -1: the device sent no code
};
struct ContactStart {
    uint32_t count = 0;
};
struct Contact {
    PubKey key{};
    uint8_t type = 0;
    uint8_t flags = 0;
    uint8_t out_path_len = kPathFlood;
    std::array<uint8_t, 64> out_path{};
    std::string name;
    uint32_t last_advert = 0;               // the node's own clock
    double lat = 0, lon = 0;
    uint32_t lastmod = 0;
};
struct ContactEnd {
    uint32_t lastmod = 0;
};
struct SelfInfo {
    uint8_t adv_type = 0;
    int8_t tx_power = 0;                    // dBm, signed (meshcore_py reads it unsigned: a negative power would show as 2xx)
    uint8_t max_tx_power = 0;
    PubKey key{};
    double lat = 0, lon = 0;
    uint8_t multi_acks = 0;
    uint8_t adv_loc_policy = 0;
    uint8_t telemetry_mode = 0;
    bool manual_add_contacts = false;
    uint32_t freq_khz = 0;                  // the wire carries kHz (frequency in MHz times 1000)
    uint32_t bw_hz = 0;                     // the wire carries Hz (bandwidth in kHz times 1000)
    uint8_t sf = 0;
    uint8_t cr = 0;
    std::string name;

    double freq_mhz() const { return freq_khz / 1000.0; }
    double bw_khz() const { return bw_hz / 1000.0; }
};
struct MsgSent {
    uint8_t type = 0;                       // 1 = flood, 0 = direct
    std::array<uint8_t, 4> expected_ack{};
    uint32_t suggested_timeout_ms = 0;
};
struct IncomingMessage {
    bool channel = false;
    uint8_t channel_idx = 0;
    KeyPrefix prefix{};                     // sender, direct messages only
    uint8_t path_len = kPathFlood;
    uint8_t txt_type = 0;
    uint32_t sender_timestamp = 0;          // the sender's clock
    bool has_snr = false;
    double snr = 0;                         // dB (V3 frames)
    std::string text;
};
struct CurrentTime {
    uint32_t time = 0;
};
struct NoMoreMsgs {};
struct Battery {
    uint16_t millivolts = 0;
    bool has_storage = false;
    uint32_t used_kb = 0, total_kb = 0;
};
struct DeviceInfo {
    uint8_t fw_ver = 0;                     // protocol level of the firmware, not a version string
    uint8_t max_contacts = 0;               // already multiplied by 2
    uint8_t max_channels = 0;
    uint32_t ble_pin = 0;
    std::string fw_build, model, version;
    bool has_repeat = false;
    bool repeat = false;
    int path_hash_mode = -1;
    int max_contacts_value() const { return max_contacts * 2; }
};
struct ChannelInfo {
    uint8_t idx = 0;
    std::string name;
    std::array<uint8_t, 16> secret{};
    bool empty() const;
};
struct Advertisement {
    PubKey key{};
};
struct PathUpdate {
    PubKey key{};
};
struct Ack {
    std::array<uint8_t, 4> code{};
    bool has_trip = false;
    uint32_t trip_ms = 0;
};
struct MessagesWaiting {};
/* RESP_CODE_CUSTOM_VARS: "key:value,key:value" text (meshcore_py reader.py). */
struct CustomVars {
    std::vector<std::pair<std::string, std::string>> vars;
    /* The value of a key (case-insensitive), or nullptr. */
    const std::string *find(const std::string &key) const;
};
struct LogData {
    double snr = 0;
    int rssi = 0;
};
struct NewAdvert {
    Contact contact;
};
struct ContactDeleted {
    PubKey key{};
};
struct ContactsFull {};
struct Unknown {
    uint8_t code = 0;
    size_t length = 0;
};

using Packet = std::variant<Ok, Error, ContactStart, Contact, ContactEnd, SelfInfo, MsgSent, IncomingMessage, CurrentTime, CustomVars,
                            NoMoreMsgs, Battery, DeviceInfo, ChannelInfo, Advertisement, PathUpdate, Ack, MessagesWaiting,
                            LogData, NewAdvert, ContactDeleted, ContactsFull, Unknown>;

/* Decodes one frame payload. nullopt for an empty or truncated frame (too short to hold its fixed fields). Unknown
 * codes decode to Unknown so a newer firmware never breaks the stream. */
std::optional<Packet> parse_packet(const Bytes &frame);
/* The first byte of a frame (0 for an empty one). */
inline uint8_t packet_code(const Bytes &frame) { return frame.empty() ? 0 : frame[0]; }

/* ------------------------------------------------------------------ command builders (payloads, not framed) */

Bytes build_device_query();
Bytes build_app_start(const std::string &app_name);
Bytes build_get_contacts(uint32_t since = 0);
Bytes build_get_contact_by_key(const PubKey &key);
Bytes build_set_time(uint32_t unix_time);
Bytes build_send_advert(bool flood);
Bytes build_set_name(const std::string &name);
Bytes build_sync_next();
Bytes build_get_time();
Bytes build_get_custom_vars();
/* SET_CUSTOM_VAR: the payload is the text "key:value" (meshcore_py set_custom_var). */
Bytes build_set_custom_var(const std::string &key, const std::string &value);
Bytes build_battery();
Bytes build_get_channel(uint8_t idx);
Bytes build_set_channel(uint8_t idx, const std::string &name, const std::array<uint8_t, 16> &secret);
Bytes build_reset_path(const PubKey &key);
/* freq_mhz e.g. 869.525, bw_khz e.g. 250 or 62.5; the values are rounded to the wire units (kHz, Hz). */
Bytes build_set_radio(double freq_mhz, double bw_khz, uint8_t sf, uint8_t cr);
Bytes build_set_tx_power(int dbm);
/* Direct message to the first 6 bytes of a contact's public key. */
Bytes build_send_txt(const KeyPrefix &dst, const std::string &text, uint32_t timestamp, uint8_t attempt);
Bytes build_send_channel_txt(uint8_t channel_idx, const std::string &text, uint32_t timestamp);

/* Largest text that is safe to send (the firmware's text field is limited; the doc says 133 characters). */
constexpr size_t kMaxDirectText = 133;
constexpr size_t kMaxNameBytes = 31;
/* The well known key of the public channel (companion_protocol.md, "Public Channel"). */
extern const std::array<uint8_t, 16> kPublicChannelSecret;

} // namespace meshzero
