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
constexpr uint8_t kAddUpdateContact = 9;
constexpr uint8_t kSyncNext = 10;
constexpr uint8_t kGetCustomVars = 40;
constexpr uint8_t kSetCustomVar = 41;
constexpr uint8_t kSetRadio = 11;
constexpr uint8_t kSetTxPower = 12;
constexpr uint8_t kResetPath = 13;
constexpr uint8_t kSetAdvertLatLon = 14;
constexpr uint8_t kRemoveContact = 15;
constexpr uint8_t kReboot = 19;
constexpr uint8_t kBattery = 20;
constexpr uint8_t kDeviceQuery = 22;
constexpr uint8_t kSendLogin = 26;
constexpr uint8_t kLogout = 29;
constexpr uint8_t kGetContactByKey = 30;
constexpr uint8_t kGetChannel = 31;
constexpr uint8_t kSetChannel = 32;
constexpr uint8_t kSetOtherParams = 38;
constexpr uint8_t kSendBinaryReq = 50;                 // companion v1.8.0+ (MyMesh.cpp CMD_SEND_BINARY_REQ)
constexpr uint8_t kFactoryReset = 51;
constexpr uint8_t kSetFloodScope = 54;
constexpr uint8_t kSendControlData = 55;
constexpr uint8_t kGetStats = 56;
constexpr uint8_t kSendAnonReq = 57;                   // companion v1.12.0+ (CMD_SEND_ANON_REQ)
constexpr uint8_t kSetAutoadd = 58;
constexpr uint8_t kGetAutoadd = 59;
constexpr uint8_t kGetAllowedRepeatFreq = 60;
constexpr uint8_t kSetPathHashMode = 61;
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
constexpr uint8_t kStats = 24;
constexpr uint8_t kAutoaddConfig = 25;
constexpr uint8_t kAllowedRepeatFreq = 26;
constexpr uint8_t kChannelData = 27;
// pushes
constexpr uint8_t kAdvertisement = 0x80;
constexpr uint8_t kPathUpdate = 0x81;
constexpr uint8_t kAck = 0x82;
constexpr uint8_t kMessagesWaiting = 0x83;
constexpr uint8_t kLoginSuccess = 0x85;
constexpr uint8_t kLoginFail = 0x86;
constexpr uint8_t kLogData = 0x88;
constexpr uint8_t kNewAdvert = 0x8A;
constexpr uint8_t kBinaryResponse = 0x8C;
constexpr uint8_t kControlData = 0x8E;
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

/* Request types of a binary request (CMD 50) to a repeater or room: the first byte of the request data (MeshCore 81dbae5
 * examples/simple_repeater/MyMesh.cpp:46-51; meshcore_py packets.py BinaryReqType has 0x01-0x06, not 0x07). */
namespace reqtype {
constexpr uint8_t kStatus = 0x01;                      // guests too (MyMesh.cpp:216)
constexpr uint8_t kKeepAlive = 0x02;
constexpr uint8_t kTelemetry = 0x03;                   // a guest gets the base only: battery voltage, MCU temperature (:240-250)
constexpr uint8_t kMinMaxAvg = 0x04;                   // sensors only
constexpr uint8_t kAccessList = 0x05;                  // admin only (:262)
constexpr uint8_t kNeighbours = 0x06;                  // repeaters only, any logged-in client (:276-371)
constexpr uint8_t kOwnerInfo = 0x07;                   // repeater FIRMWARE_VER_LEVEL >= 2 (:51, :372-375)
} // namespace reqtype
/* Types of an anonymous request (CMD 57): no login needed, answered only when the request arrives DIRECT (zero hop or a known
 * path; MyMesh.cpp:584-589), at most 4 per 3 minutes per repeater for everybody (:869 anon_limiter(4, 180)). Repeaters of level 2. */
namespace anontype {
constexpr uint8_t kRegions = 0x01;
constexpr uint8_t kOwner = 0x02;
constexpr uint8_t kBasic = 0x03;                       // the clock and a feature byte
} // namespace anontype
/* The repeater's FIRMWARE_VER_LEVEL (byte 13 of a LOGIN_SUCCESS from companion v1.10.0) from which it answers: NEIGHBOURS from 1
 * (repeater v1.9.0; before that the request is unknown and gets no answer), OWNER_INFO 0x07, the anonymous requests and the receive
 * error counter of the status from 2 (repeater v1.12.0). Before v1.9.0 the byte is the zero padding of the reply: level 0. */
constexpr int kRepeaterLevelNeighbours = 1;
constexpr int kRepeaterLevelOwnerInfo = 2;
/* NEIGHBOURS: the key prefix length asked for (meshcore_py's default) and the size of the repeater's result buffer
 * (MyMesh.cpp:342 results_buffer[130]: 14 entries of 4 + 4 + 1 bytes per page). */
constexpr size_t kNeighbourPrefixLen = 4;
constexpr size_t kNeighboursResultBytes = 130;
/* NEIGHBOURS order_by (MyMesh.cpp:287). */
enum class NeighbourOrder : uint8_t { NewestFirst = 0, OldestFirst = 1, StrongestFirst = 2, WeakestFirst = 3 };

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
    Bytes raw;                              // the packet as it came over the air (header, path, payload); may be empty
};
struct NewAdvert {
    Contact contact;
};
struct ContactDeleted {
    PubKey key{};
};
struct ContactsFull {};
/* PUSH_CODE_LOGIN_SUCCESS (0x85): a repeater or room accepted our login (companion MyMesh.cpp onContactResponse; meshcore_py reader.py).
 * 8 bytes up to companion v1.9 (level 7 and below): the code, `permissions`, the 6 byte key prefix of the remote node. From companion
 * v1.10.0 (level 8) 14 bytes: + the remote node's clock (4), its ACL permissions byte, its FIRMWARE_VER_LEVEL. The push carries no
 * request tag: it answers the login the companion has pending (one at a time), matched here by the key prefix. */
struct LoginSuccess {
    uint8_t permissions = 0;                // byte 1: 1 = admin (a room: 1 admin, 2 read only, 0 read-write); 0 in the legacy "OK" reply
    KeyPrefix prefix{};
    bool has_clock = false;
    uint32_t clock = 0;                     // the remote node's clock when it answered (UNIX time)
    bool has_acl = false;
    uint8_t acl = 0;                        // its ACL permissions: the role in bits 0-1 (0 guest, 1 read only, 2 read-write, 3 admin)
    bool has_fw_level = false;
    uint8_t fw_level = 0;                   // the remote firmware's own level (a repeater: 2 has the owner info request)
};
/* PUSH_CODE_LOGIN_FAIL (0x86): only for a legacy reply. A current repeater or room answers a wrong password with nothing at all. */
struct LoginFail {
    KeyPrefix prefix{};
};
/* PUSH_CODE_BINARY_RESPONSE (0x8C, companion MyMesh.cpp onContactResponse:750-760): the code, a reserved byte, the 4 byte tag that SENT
 * gave for the binary or anonymous request, then the reply of the remote node after its own copy of the tag. The companion keeps ONE
 * pending tag (clearPendingReqs); a reply it no longer waits for comes with the same layout (:761-776: the raw reply, whose first 4
 * bytes are its tag), so the tag decides. The data ends with the zero padding of the decryption (16 byte blocks: Utils.cpp encrypt). */
struct BinaryResponse {
    std::array<uint8_t, 4> tag{};
    Bytes data;
};

/* RESP_CODE_STATS (24), three sub types (meshcore_py reader.py): core 11 bytes, radio 14, packets 26 or 30. */
struct CoreStats {
    uint16_t battery_mv = 0;
    uint32_t uptime_secs = 0;
    uint16_t errors = 0;
    uint8_t queue_len = 0;
};
struct RadioStats {
    int noise_floor = 0;                    // dBm
    int last_rssi = 0;
    double last_snr = 0;                    // dB (the wire carries SNR times 4)
    uint32_t tx_air_secs = 0;
    uint32_t rx_air_secs = 0;
};
struct PacketStats {
    uint32_t recv = 0, sent = 0, flood_tx = 0, direct_tx = 0, flood_rx = 0, direct_rx = 0;
    bool has_errors = false;                // the legacy 26 byte frame has no error counter
    uint32_t recv_errors = 0;
};
struct StatsReply {
    enum Kind { Core = 0, Radio = 1, Packets = 2 } kind = Core;
    CoreStats core;
    RadioStats radio;
    PacketStats packets;
};
/* RESP_CODE_AUTOADD_CONFIG (25): bit 0 overwrite the oldest non-favourite when full, bits 1..4 the types added by themselves when
 * manual add is on (chat, repeater, room, sensor). max_hops exists from companion v1.14. */
struct AutoaddConfig {
    uint8_t config = 0;
    bool has_max_hops = false;
    uint8_t max_hops = 0;
};
constexpr uint8_t kAutoaddOverwrite = 0x01, kAutoaddChat = 0x02, kAutoaddRepeater = 0x04, kAutoaddRoom = 0x08, kAutoaddSensor = 0x10;
/* RESP_CODE_ALLOWED_REPEAT_FREQ (26): pairs of u32 (lower, upper) ended by a zero; kHz like the radio frequency of SET_RADIO. */
struct RepeatRange {
    uint32_t lo_khz = 0, hi_khz = 0;
};
struct AllowedRepeatFreq {
    std::vector<RepeatRange> ranges;
};
/* PUSH_CODE_CONTROL_DATA (0x8E): SNR, RSSI, path length and the control payload. A DISCOVER_RESP (flags 0x9n) is decoded further. */
struct ControlData {
    double snr = 0;
    int rssi = 0;
    uint8_t path_len = 0;
    Bytes payload;
    bool discover = false;                  // payload[0] & 0xF0 == 0x90 and the rest is well formed
    uint8_t node_type = 0;                  // lower nibble of the flags: an ADV_TYPE_*
    double snr_in = 0;                      // how the node heard our request
    std::array<uint8_t, 4> tag{};
    Bytes pubkey;                           // 32 bytes, or an 8 byte prefix
};
struct Unknown {
    uint8_t code = 0;
    size_t length = 0;
};

using Packet = std::variant<Ok, Error, ContactStart, Contact, ContactEnd, SelfInfo, MsgSent, IncomingMessage, CurrentTime, CustomVars,
                            NoMoreMsgs, Battery, DeviceInfo, ChannelInfo, Advertisement, PathUpdate, Ack, MessagesWaiting,
                            LogData, NewAdvert, ContactDeleted, ContactsFull, StatsReply, AutoaddConfig, AllowedRepeatFreq, ControlData,
                            LoginSuccess, LoginFail, BinaryResponse, Unknown>;

/* ------------------------------------------------------------------ the replies of a repeater or room (report 036, slice S2)
 * Read from BinaryResponse::data (the bytes after the tag). Trailing zero padding is allowed everywhere. */

/* STATUS (0x01). A repeater answers RepeaterStats (simple_repeater MyMesh.h:44-60): 56 bytes from v1.12.0, 52 before (no
 * n_recv_errors); meshcore_py parsing.py parse_status reads the same offsets. A room answers ServerStats (simple_room_server
 * MyMesh.cpp:24-39): the same first 48 bytes, then n_posted and n_post_push (u16 each) where a repeater has its rx airtime;
 * meshcore_py does not tell them apart. The padding makes a 52 byte repeater reply look 56 long (the error counter then reads 0):
 * the client drops has_rx_errors when the node's level is known to be below kRepeaterLevelOwnerInfo. */
struct RepeaterStatus {
    bool room = false;                      // read with the room layout
    uint16_t battery_mv = 0;
    uint16_t tx_queue = 0;                  // packets waiting to be sent
    int noise_floor = 0;                    // dBm
    int last_rssi = 0;                      // dBm
    double last_snr = 0;                    // dB (the wire carries SNR x 4, int16)
    uint32_t packets_rx = 0, packets_tx = 0;
    uint32_t airtime_tx_s = 0;              // total transmit airtime, seconds
    uint32_t uptime_s = 0;
    uint32_t flood_tx = 0, direct_tx = 0, flood_rx = 0, direct_rx = 0;
    uint16_t debug_flags = 0;               // err_events (_err_flags; meshcore_py "full_evts"): the app's "Debug Flags"
    uint16_t direct_dups = 0, flood_dups = 0;
    bool has_airtime_rx = false;            // a repeater
    uint32_t airtime_rx_s = 0;
    bool has_rx_errors = false;             // a repeater, 56 bytes
    uint32_t rx_errors = 0;
    uint16_t posted = 0, post_pushes = 0;   // a room
};
constexpr size_t kStatusBytes = 52;
std::optional<RepeaterStatus> parse_repeater_status(const Bytes &data, bool room = false);

/* NEIGHBOURS (0x06, MyMesh.cpp:276-371): the repeaters this one hears at zero hop. The reply: the total (int16), the count in this
 * page (int16), then per entry the key prefix (as long as asked), heard N seconds ago (u32, by the repeater's clock), SNR x 4 (int8). */
struct NeighbourEntry {
    Bytes prefix;
    uint32_t heard_secs_ago = 0;
    double snr = 0;                         // dB
};
struct NeighboursPage {
    int total = 0;                          // neighbours the repeater knows
    uint16_t offset = 0;                    // the first one in this page (as asked)
    std::vector<NeighbourEntry> entries;
    bool more = false;                      // offset + entries < total (and this page was not empty)
};
std::optional<NeighboursPage> parse_neighbours(const Bytes &data, size_t prefix_len, uint16_t offset);

/* OWNER_INFO (0x07, MyMesh.cpp:372-375): "<FIRMWARE_VERSION>\n<node name>\n<owner info>". The anonymous OWNER (0x02, :165-182): the
 * node's clock (u32), then "<node name>\n<owner info>". The owner info may hold new lines (`set owner.info` turns '|' into '\n',
 * CommonCLI.cpp:577-584; 119 bytes at most). */
struct OwnerInfo {
    bool anonymous = false;                 // read from the anonymous answer: no firmware version
    std::string fw_version;                 // e.g. "v1.18.0"
    std::string name;
    std::string owner_info;
    bool has_clock = false;                 // the anonymous answer carries the node's clock
    uint32_t clock = 0;
};
std::optional<OwnerInfo> parse_owner_info(const Bytes &data);
std::optional<OwnerInfo> parse_anon_owner(const Bytes &data);
/* The anonymous BASIC (0x03, MyMesh.cpp:184-209): the node's clock and a feature byte: 0x80 repeat off, low bits 0x01 a UART
 * bridge, 0x03 an ESP-NOW bridge. */
struct AnonBasic {
    uint32_t clock = 0;
    uint8_t features = 0;
    bool repeat_disabled() const { return (features & 0x80) != 0; }
    uint8_t bridge() const { return features & 0x03; }
};
std::optional<AnonBasic> parse_anon_basic(const Bytes &data);

/* An ADVERT packet read out of a LogData frame (the radio log of every packet the board hears): who advertised, what it is, where it
 * is, how loud it was and how many hops it took. The Ed25519 signature is NOT checked (the app has no crypto for it): a name and a
 * position shown from here can be forged by anybody on the air, like in every client that does not verify. */
struct HeardAdvert {
    PubKey key{};
    uint8_t type = 0;                       // ADV_TYPE_* from the appdata flags (0 when the flags carry none)
    std::string name;
    bool has_pos = false;
    double lat = 0, lon = 0;
    int hops = -1;                          // hop count of the path the packet took; -1 when the route is unreadable
    int hash_size = 1;                      // bytes per path hash (1..3)
    double snr = 0;
    int rssi = 0;
    uint32_t timestamp = 0;                 // the advertiser's clock
};
std::optional<HeardAdvert> parse_heard_advert(const LogData &log);

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
/* freq_mhz e.g. 869.525, bw_khz e.g. 250 or 62.5; the values are rounded to the wire units (kHz, Hz). repeat: -1 = no trailing byte (the
 * radio parameters only); 0 / 1 = the trailing "client repeat" byte of firmware level 9 and up (meshcore_py set_radio). */
Bytes build_set_radio(double freq_mhz, double bw_khz, uint8_t sf, uint8_t cr, int repeat = -1);
Bytes build_set_tx_power(int dbm);
/* SET_ADVERT_LATLON (meshcore_py set_coords): 0E, latitude and longitude as signed 32-bit microdegrees, then a 32-bit altitude (0; the
 * firmware reads it "for future support"). The firmware answers OK, or ERROR illegal arg outside -90..90 / -180..180. 0, 0 = no position. */
Bytes build_set_advert_latlon(double lat, double lon);
/* REMOVE_CONTACT: the 32 byte public key. The board answers OK (or ERROR not found). */
Bytes build_remove_contact(const PubKey &key);
/* ADD_UPDATE_CONTACT (meshcore_py update_contact): key, type, flags, path length byte, 64 byte path, 32 byte name, last advert, lat, lon. */
Bytes build_add_update_contact(const Contact &c);
/* REBOOT: the command byte and the text "reboot" (meshcore_py reboot; the firmware checks the word). No answer comes: the link drops. */
Bytes build_reboot();
/* FACTORY_RESET: the command byte 51 and the word "reset" (the reference client sends the byte alone; see the comment in the .cpp). Erases the
 * keys, contacts and channels of the board. */
Bytes build_factory_reset();
/* GET_STATS with the sub type 0 core, 1 radio, 2 packets. */
Bytes build_get_stats(uint8_t sub_type);
/* SET_OTHER_PARAMS in the 5 byte form of meshcore_py set_other_params_from_infos (manual add, telemetry mode byte, advert location policy,
 * multi acks); with_multi_acks false gives the older 4 byte form. */
Bytes build_set_other_params(bool manual_add, uint8_t telemetry_mode, uint8_t adv_loc_policy, uint8_t multi_acks, bool with_multi_acks = true);
Bytes build_set_autoadd_config(uint8_t flags);
Bytes build_get_autoadd_config();
Bytes build_get_allowed_repeat_freq();
/* SET_PATH_HASH_MODE: 0, 1, 2 = 1, 2, 3 bytes per hop hash. Frame 3D 00 mode (meshcore_py set_path_hash_mode). */
Bytes build_set_path_hash_mode(int mode);
/* SET_FLOOD_SCOPE back to the default (no scope): 36 00 and a zero 16 byte key, as meshcore_py set_flood_scope("") sends it. Mesh Hop
 * never sets a scope; it only undoes one a meshcore-cli session left (report 033). An older firmware answers "unsupported". */
Bytes build_reset_flood_scope();
/* SEND_CONTROL_DATA carrying a DISCOVER_REQ (meshcore_py send_node_discover_req): flags 0x80 | prefix_only, the node type filter (one bit
 * per ADV_TYPE: bit n for type n, 0xFF = all), a 4 byte tag. Zero-hop only: the answers come from the nodes in direct radio range. */
Bytes build_discover_request(uint8_t type_filter, uint32_t tag, bool prefix_only);
constexpr uint8_t kDiscoverAllTypes = 0xFF;
/* SEND_LOGIN (26, meshcore_py send_login): the full 32 byte key of a contact (a repeater or room), then the password bytes (none for a
 * blank, guest login). The companion answers SENT (6: flood flag, tag = the first 4 key bytes, est_timeout in ms), ERROR not found (not a
 * contact) or table full (nothing could be sent); the verdict comes later as a push (LoginSuccess, rarely LoginFail) or never. The firmware
 * cuts a password at kMaxLoginPassword bytes (BaseChatMesh::sendLogin): longer ones are refused before this is built. */
Bytes build_send_login(const PubKey &key, const std::string &password);
constexpr size_t kMaxLoginPassword = 15;
/* LOGOUT (29): the 32 byte key. The companion only stops its keep-alive to that node and answers OK; the remote node keeps us in its
 * access list. */
Bytes build_logout(const PubKey &key);
/* SEND_BINARY_REQ (50, companion MyMesh.cpp:1720-1740; meshcore_py base.py send_binary_req): the full 32 byte key of a contact, the
 * request type, its parameters. The companion answers SENT (6: a flood flag, the TAG to match the push 0x8C with, est_timeout in ms),
 * ERROR not found (not a contact) or table full (nothing sent). The remote node answers only a client in its access list (a login,
 * guest or admin), and nothing to an unknown type. */
Bytes build_binary_request(const PubKey &key, uint8_t req_type, const Bytes &params = {});
Bytes build_status_request(const PubKey &key);                // 50, key, 0x01 (as meshcore_py req_status_sync)
Bytes build_owner_info_request(const PubKey &key);            // 50, key, 0x07
/* 50, key, 0x03 and the four reserved bytes of the companion's own sendRequest(type) (BaseChatMesh.cpp:690-699): byte 1 is the inverse
 * permission mask (0: all sensors; a guest gets the base only whatever it asks). */
Bytes build_telemetry_request(const PubKey &key);
/* 50, key, 0x06, version 0, count (0-255; the repeater stops at its 130 byte buffer), offset (u16 LE), order, key prefix length, a
 * 4 byte nonce for the packet hash (meshcore_py binary.py req_neighbours_async). */
Bytes build_neighbours_request(const PubKey &key, uint8_t count, uint16_t offset, NeighbourOrder order, uint8_t prefix_len, uint32_t nonce);
/* SEND_ANON_REQ (57, companion MyMesh.cpp:1599-1629; meshcore_py base.py send_anon_req): the key, the anonymous type, then the reply
 * path the remote node must answer on (anon_reply_path). Answered SENT like 50 (the tag is the companion's clock, BaseChatMesh.cpp:642). */
Bytes build_anon_request(const PubKey &key, uint8_t anon_type, const Bytes &reply_path);
/* The reply path of an anonymous request (meshcore_py encode_reply_path): the length byte of the contact's out path (hops | hash
 * mode << 6), then the path reversed hop by hop. nullopt for a flood route (no path: the request would flood and the repeater ignores an
 * anonymous request that is not direct) or a length byte the firmware rejects (Packet::isValidPathLen: hash size 4, over 64 bytes). */
std::optional<Bytes> anon_reply_path(uint8_t out_path_len, const std::array<uint8_t, 64> &out_path);
/* Direct message to the first 6 bytes of a contact's public key. */
Bytes build_send_txt(const KeyPrefix &dst, const std::string &text, uint32_t timestamp, uint8_t attempt);
Bytes build_send_channel_txt(uint8_t channel_idx, const std::string &text, uint32_t timestamp);

/* Largest text that is safe to send (the firmware's text field is limited; the doc says 133 characters). */
constexpr size_t kMaxDirectText = 133;
constexpr size_t kMaxNameBytes = 31;
/* The well known key of the public channel (companion_protocol.md, "Public Channel"). */
extern const std::array<uint8_t, 16> kPublicChannelSecret;

} // namespace meshzero
