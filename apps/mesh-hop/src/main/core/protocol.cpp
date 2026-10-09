/*
 * SPDX-License-Identifier: MIT
 */

#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace meshzero {

const std::array<uint8_t, 16> kPublicChannelSecret = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                                      0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

std::string error_text(int code)
{
    switch (code) {
    case err::kUnsupported: return "command not supported by this firmware";
    case err::kNotFound: return "not found on the radio";
    case err::kTableFull: return "radio queue full, try again";
    case err::kBadState: return "radio busy, try again";
    case err::kFileIo: return "radio storage error";
    case err::kIllegalArg: return "value refused by the radio";
    case -1: return "refused by the radio";
    default: return "radio error " + std::to_string(code);
    }
}

const char *contact_type_name(int type)
{
    switch (type) {
    case advtype::kChat: return "Chat";
    case advtype::kRepeater: return "Repeater";
    case advtype::kRoom: return "Room";
    case advtype::kSensor: return "Sensor";
    default: return "Node";
    }
}

int path_hops(uint8_t path_len)
{
    return path_len == kPathFlood ? -1 : (path_len & 0x3F);
}

int path_hash_mode(uint8_t path_len)
{
    return path_len == kPathFlood ? -1 : (path_len >> 6);
}

const std::string *CustomVars::find(const std::string &key) const
{
    auto lower = [](std::string s) {
        for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string k = lower(key);
    for (const auto &kv : vars)
        if (lower(kv.first) == k) return &kv.second;
    return nullptr;
}

bool ChannelInfo::empty() const
{
    if (!name.empty()) return false;
    for (uint8_t b : secret)
        if (b != 0) return false;
    return true;
}

namespace {

/* Bounds-checked little endian reader over a frame. Reads past the end set fail_ and return zeros. */
class Reader {
public:
    explicit Reader(const Bytes &b, size_t pos = 1) : b_(b), pos_(pos) {}
    size_t left() const { return pos_ <= b_.size() ? b_.size() - pos_ : 0; }
    bool failed() const { return fail_; }
    uint8_t u8()
    {
        if (left() < 1) { fail_ = true; return 0; }
        return b_[pos_++];
    }
    uint16_t u16()
    {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    void skip(size_t n)
    {
        if (left() < n) { fail_ = true; pos_ = b_.size(); return; }
        pos_ += n;
    }
    template <size_t N> std::array<uint8_t, N> arr()
    {
        std::array<uint8_t, N> a{};
        if (left() < N) { fail_ = true; return a; }
        std::copy_n(b_.begin() + static_cast<long>(pos_), N, a.begin());
        pos_ += N;
        return a;
    }
    /* Fixed width text field: up to the first NUL, invalid UTF-8 replaced. */
    std::string fixed_text(size_t n)
    {
        const size_t take = std::min(n, left());
        if (take < n) fail_ = true;
        std::string s(reinterpret_cast<const char *>(b_.data() + pos_), take);
        pos_ += take;
        const size_t nul = s.find('\0');
        if (nul != std::string::npos) s.resize(nul);
        return sanitize_utf8(s);
    }
    /* The rest of the frame as text; trailing NULs (padding) are cut. */
    std::string rest_text()
    {
        std::string s(reinterpret_cast<const char *>(b_.data() + std::min(pos_, b_.size())), left());
        pos_ = b_.size();
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return sanitize_utf8(s);
    }

private:
    const Bytes &b_;
    size_t pos_;
    bool fail_ = false;
};

/* 148 bytes with the code: key 32, type, flags, path_len, path 64, name 32, last_advert, lat, lon, lastmod. The
 * last field is optional here (a frame cut after lon still decodes) because only the identity matters. */
std::optional<Contact> parse_contact_body(const Bytes &f)
{
    if (f.size() < 1 + 32 + 3 + 64 + 32 + 4 + 8) return std::nullopt;
    Reader r(f);
    Contact c;
    c.key = r.arr<32>();
    c.type = r.u8();
    c.flags = r.u8();
    c.out_path_len = r.u8();
    c.out_path = r.arr<64>();
    c.name = r.fixed_text(32);
    c.last_advert = r.u32();
    c.lat = r.i32() / 1e6;
    c.lon = r.i32() / 1e6;
    if (r.left() >= 4) c.lastmod = r.u32();
    if (r.failed()) return std::nullopt;
    return c;
}

void put_u32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

} // namespace

std::optional<Packet> parse_packet(const Bytes &f)
{
    if (f.empty()) return std::nullopt;
    const uint8_t code = f[0];
    Reader r(f);
    switch (code) {
    case resp::kOk: {
        Ok o;
        if (f.size() == 5) {          // meshcore_py: only an exact 5 byte frame carries a value
            o.has_value = true;
            o.value = r.u32();
        }
        return o;
    }
    case resp::kError: {
        Error e;
        if (f.size() > 1) e.code = f[1];
        return e;
    }
    case resp::kContactStart: {
        if (f.size() < 5) return std::nullopt;
        ContactStart s;
        s.count = r.u32();
        return s;
    }
    case resp::kContact: {
        auto c = parse_contact_body(f);
        if (!c) return std::nullopt;
        return *c;
    }
    case resp::kNewAdvert: {
        auto c = parse_contact_body(f);
        if (!c) return std::nullopt;
        return NewAdvert{*c};
    }
    case resp::kContactEnd: {
        ContactEnd e;
        if (f.size() >= 5) e.lastmod = r.u32();
        return e;
    }
    case resp::kSelfInfo: {
        if (f.size() < 58) return std::nullopt;
        SelfInfo s;
        s.adv_type = r.u8();
        s.tx_power = r.i8();
        s.max_tx_power = r.u8();
        s.key = r.arr<32>();
        s.lat = r.i32() / 1e6;          // signed: companion_protocol.md's pseudocode reads these as unsigned (doc differs)
        s.lon = r.i32() / 1e6;
        s.multi_acks = r.u8();
        s.adv_loc_policy = r.u8();
        s.telemetry_mode = r.u8();
        s.manual_add_contacts = r.u8() > 0;
        s.freq_khz = r.u32();
        s.bw_hz = r.u32();
        s.sf = r.u8();
        s.cr = r.u8();
        s.name = r.rest_text();
        return s;
    }
    case resp::kMsgSent: {
        if (f.size() < 10) return std::nullopt;
        MsgSent m;
        m.type = r.u8();
        m.expected_ack = r.arr<4>();
        m.suggested_timeout_ms = r.u32();
        return m;
    }
    case resp::kContactMsg:
    case resp::kContactMsgV3: {
        IncomingMessage m;
        if (code == resp::kContactMsgV3) {
            if (f.size() < 16) return std::nullopt;
            m.has_snr = true;
            m.snr = r.i8() / 4.0;
            r.skip(2);
        } else if (f.size() < 13) {
            return std::nullopt;
        }
        m.prefix = r.arr<6>();
        m.path_len = r.u8();
        m.txt_type = r.u8();
        m.sender_timestamp = r.u32();
        if (m.txt_type == txttype::kSignedPlain) r.skip(4);     // 4 byte signature
        m.text = r.rest_text();
        if (r.failed()) return std::nullopt;
        return m;
    }
    case resp::kChannelMsg:
    case resp::kChannelMsgV3: {
        IncomingMessage m;
        m.channel = true;
        if (code == resp::kChannelMsgV3) {
            if (f.size() < 11) return std::nullopt;
            m.has_snr = true;
            m.snr = r.i8() / 4.0;
            r.skip(2);
        } else if (f.size() < 8) {
            return std::nullopt;
        }
        m.channel_idx = r.u8();
        m.path_len = r.u8();
        m.txt_type = r.u8();
        m.sender_timestamp = r.u32();
        m.text = r.rest_text();
        if (r.failed()) return std::nullopt;
        return m;
    }
    case resp::kCurrentTime: {
        if (f.size() < 5) return std::nullopt;
        return CurrentTime{r.u32()};
    }
    case resp::kCustomVars: {
        CustomVars cv;
        const std::string text = r.rest_text();
        size_t pos = 0;
        while (pos <= text.size() && !text.empty()) {
            size_t comma = text.find(',', pos);
            if (comma == std::string::npos) comma = text.size();
            const std::string pair = text.substr(pos, comma - pos);
            const size_t colon = pair.find(':');
            if (colon != std::string::npos) cv.vars.emplace_back(pair.substr(0, colon), pair.substr(colon + 1));
            pos = comma + 1;
        }
        return cv;
    }
    case resp::kNoMoreMsgs: return NoMoreMsgs{};
    case resp::kBattery: {
        if (f.size() < 3) return std::nullopt;
        Battery b;
        b.millivolts = r.u16();
        if (f.size() >= 11) {           // meshcore_py: a shorter frame has no storage figures
            b.has_storage = true;
            b.used_kb = r.u32();
            b.total_kb = r.u32();
        }
        return b;
    }
    case resp::kDeviceInfo: {
        DeviceInfo d;
        if (f.size() < 2) return std::nullopt;
        d.fw_ver = r.u8();
        if (d.fw_ver >= 3) {
            if (f.size() < 80) return std::nullopt;
            d.max_contacts = r.u8();
            d.max_channels = r.u8();
            d.ble_pin = r.u32();
            d.fw_build = r.fixed_text(12);
            d.model = r.fixed_text(40);
            d.version = r.fixed_text(20);
        }
        if (d.fw_ver >= 9 && r.left() >= 1) {
            d.has_repeat = true;
            d.repeat = r.u8() != 0;
        }
        if (d.fw_ver >= 10 && r.left() >= 1) d.path_hash_mode = r.u8();
        return d;
    }
    case resp::kChannelInfo: {
        if (f.size() < 1 + 1 + 32 + 16) return std::nullopt;
        ChannelInfo c;
        c.idx = r.u8();
        c.name = r.fixed_text(32);
        c.secret = r.arr<16>();
        return c;
    }
    case resp::kAdvertisement: {
        if (f.size() < 33) return std::nullopt;
        return Advertisement{r.arr<32>()};
    }
    case resp::kPathUpdate: {
        if (f.size() < 33) return std::nullopt;
        return PathUpdate{r.arr<32>()};
    }
    case resp::kAck: {
        // meshcore_py: 4 byte ack code, then (since firmware 1.0.0a) a 4 byte round trip time. The doc shows a
        // 6 byte code (doc differs).
        if (f.size() < 5) return std::nullopt;
        Ack a;
        a.code = r.arr<4>();
        if (f.size() >= 9) {
            a.has_trip = true;
            a.trip_ms = r.u32();
        }
        return a;
    }
    case resp::kMessagesWaiting: return MessagesWaiting{};
    case resp::kLogData: {
        LogData l;
        if (f.size() >= 3) {
            l.snr = r.i8() / 4.0;
            l.rssi = r.i8();
            if (f.size() > 3) l.raw.assign(f.begin() + 3, f.end());
        }
        return l;
    }
    case resp::kStats: {
        if (f.size() < 2) return std::nullopt;
        StatsReply s;
        const uint8_t sub = r.u8();
        if (sub == 0) {
            if (f.size() < 11) return std::nullopt;
            s.kind = StatsReply::Core;
            s.core.battery_mv = r.u16();
            s.core.uptime_secs = r.u32();
            s.core.errors = r.u16();
            s.core.queue_len = r.u8();
        } else if (sub == 1) {
            if (f.size() < 14) return std::nullopt;
            s.kind = StatsReply::Radio;
            s.radio.noise_floor = static_cast<int16_t>(r.u16());
            s.radio.last_rssi = r.i8();
            s.radio.last_snr = r.i8() / 4.0;
            s.radio.tx_air_secs = r.u32();
            s.radio.rx_air_secs = r.u32();
        } else if (sub == 2) {
            if (f.size() < 26) return std::nullopt;
            s.kind = StatsReply::Packets;
            s.packets.recv = r.u32();
            s.packets.sent = r.u32();
            s.packets.flood_tx = r.u32();
            s.packets.direct_tx = r.u32();
            s.packets.flood_rx = r.u32();
            s.packets.direct_rx = r.u32();
            if (f.size() >= 30) {
                s.packets.has_errors = true;
                s.packets.recv_errors = r.u32();
            }
        } else {
            Unknown u;
            u.code = code;
            u.length = f.size();
            return u;
        }
        return s;
    }
    case resp::kAutoaddConfig: {
        if (f.size() < 2) return std::nullopt;
        AutoaddConfig a;
        a.config = r.u8();
        if (f.size() >= 3) {
            a.has_max_hops = true;
            a.max_hops = r.u8();
        }
        return a;
    }
    case resp::kAllowedRepeatFreq: {
        AllowedRepeatFreq a;
        while (r.left() >= 8 && a.ranges.size() < 16) {
            RepeatRange rr;
            rr.lo_khz = r.u32();
            rr.hi_khz = r.u32();
            if (rr.lo_khz == 0 || rr.hi_khz == 0) break;           // meshcore_py: a zero ends the list
            a.ranges.push_back(rr);
        }
        return a;
    }
    case resp::kControlData: {
        if (f.size() < 4) return std::nullopt;
        ControlData c;
        c.snr = r.i8() / 4.0;
        c.rssi = r.i8();
        c.path_len = r.u8();
        c.payload.assign(f.begin() + 4, f.end());
        // DISCOVER_RESP: flags 0x9n (n = node type), snr*4, tag (4), then the key: 32 bytes, or an 8 byte prefix (reader.py)
        if (!c.payload.empty() && (c.payload[0] & 0xF0) == 0x90 && c.payload.size() >= 1 + 1 + 4 + 6) {
            c.node_type = c.payload[0] & 0x0F;
            c.snr_in = static_cast<int8_t>(c.payload[1]) / 4.0;
            std::copy_n(c.payload.begin() + 2, 4, c.tag.begin());
            const size_t have = c.payload.size() - 6;
            c.pubkey.assign(c.payload.begin() + 6, c.payload.begin() + 6 + static_cast<long>(have >= 32 ? 32 : std::min<size_t>(have, 8)));
            c.discover = true;
        }
        return c;
    }
    case resp::kContactDeleted: {
        if (f.size() < 33) return std::nullopt;
        return ContactDeleted{r.arr<32>()};
    }
    case resp::kContactsFull: return ContactsFull{};
    default: {
        Unknown u;
        u.code = code;
        u.length = f.size();
        return u;
    }
    }
}

/* ------------------------------------------------------------------ command builders */

Bytes build_device_query()
{
    return {cmd::kDeviceQuery, 0x03};      // 0x03 = the protocol level the app understands
}

Bytes build_app_start(const std::string &app_name)
{
    // meshcore_py sends 01 03 then 6 spaces then the app name. Byte 1 is the app protocol version (3: the
    // firmware then sends V3 message frames with the SNR); the doc calls bytes 1-7 "reserved, ignored" (doc differs).
    Bytes b = {cmd::kAppStart, 0x03, ' ', ' ', ' ', ' ', ' ', ' '};
    b.insert(b.end(), app_name.begin(), app_name.end());
    return b;
}

Bytes build_get_contacts(uint32_t since)
{
    Bytes b = {cmd::kGetContacts};
    if (since > 0) put_u32(b, since);
    return b;
}

Bytes build_get_contact_by_key(const PubKey &key)
{
    Bytes b = {cmd::kGetContactByKey};
    b.insert(b.end(), key.begin(), key.end());
    return b;
}

Bytes build_set_time(uint32_t unix_time)
{
    Bytes b = {cmd::kSetTime};
    put_u32(b, unix_time);
    return b;
}

Bytes build_send_advert(bool flood)
{
    return flood ? Bytes{cmd::kSendAdvert, 0x01} : Bytes{cmd::kSendAdvert};
}

Bytes build_set_name(const std::string &name)
{
    Bytes b = {cmd::kSetName};
    b.insert(b.end(), name.begin(), name.end());
    return b;
}

Bytes build_sync_next()
{
    return {cmd::kSyncNext};
}

Bytes build_get_time()
{
    return {cmd::kGetTime};
}

Bytes build_get_custom_vars()
{
    return {cmd::kGetCustomVars};
}

Bytes build_set_custom_var(const std::string &key, const std::string &value)
{
    Bytes b = {cmd::kSetCustomVar};
    const std::string kv = key + ":" + value;
    b.insert(b.end(), kv.begin(), kv.end());
    return b;
}

Bytes build_battery()
{
    return {cmd::kBattery};
}

Bytes build_get_channel(uint8_t idx)
{
    return {cmd::kGetChannel, idx};
}

Bytes build_set_channel(uint8_t idx, const std::string &name, const std::array<uint8_t, 16> &secret)
{
    Bytes b = {cmd::kSetChannel, idx};
    std::string n = truncate_utf8(name, 32);
    n.resize(32, '\0');
    b.insert(b.end(), n.begin(), n.end());
    b.insert(b.end(), secret.begin(), secret.end());
    return b;
}

Bytes build_reset_path(const PubKey &key)
{
    Bytes b = {cmd::kResetPath};
    b.insert(b.end(), key.begin(), key.end());
    return b;
}

Bytes build_set_radio(double freq_mhz, double bw_khz, uint8_t sf, uint8_t cr, int repeat)
{
    // meshcore_py truncates int(float(freq) * 1000); rounding avoids 869.618 becoming 869617 kHz.
    Bytes b = {cmd::kSetRadio};
    put_u32(b, static_cast<uint32_t>(std::llround(freq_mhz * 1000.0)));
    put_u32(b, static_cast<uint32_t>(std::llround(bw_khz * 1000.0)));
    b.push_back(sf);
    b.push_back(cr);
    if (repeat >= 0) b.push_back(repeat ? 1 : 0);
    return b;
}

Bytes build_set_advert_latlon(double lat, double lon)
{
    // meshcore_py truncates int(lat * 1e6); rounding keeps a typed 48.8566 at 48856600 rather than 48856599.
    Bytes b = {cmd::kSetAdvertLatLon};
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(std::llround(lat * 1e6))));
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(std::llround(lon * 1e6))));
    put_u32(b, 0);                              // altitude
    return b;
}

Bytes build_remove_contact(const PubKey &key)
{
    Bytes b = {cmd::kRemoveContact};
    b.insert(b.end(), key.begin(), key.end());
    return b;
}

Bytes build_add_update_contact(const Contact &c)
{
    Bytes b = {cmd::kAddUpdateContact};
    b.insert(b.end(), c.key.begin(), c.key.end());
    b.push_back(c.type);
    b.push_back(c.flags);
    b.push_back(c.out_path_len);
    b.insert(b.end(), c.out_path.begin(), c.out_path.end());
    std::string n = truncate_utf8(c.name, 32);
    n.resize(32, '\0');
    b.insert(b.end(), n.begin(), n.end());
    put_u32(b, c.last_advert);
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(std::llround(c.lat * 1e6))));
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(std::llround(c.lon * 1e6))));
    return b;
}

Bytes build_reboot()
{
    Bytes b = {cmd::kReboot};
    const std::string w = "reboot";
    b.insert(b.end(), w.begin(), w.end());
    return b;
}

Bytes build_factory_reset()
{
    // The companion firmware (MeshCore examples/companion_radio/MyMesh.cpp, tags companion-v1.11.0 to v1.17.1 and main) handles
    //   cmd_frame[0] == CMD_FACTORY_RESET && memcmp(&cmd_frame[1], "reset", 5) == 0
    // so the frame is the command byte 0x33 followed by the five letters "reset", exactly like REBOOT with "reboot". Without the word the command is
    // not recognised (the final else answers ERR_CODE_UNSUPPORTED_CMD). The reference client meshcore_py (commands_device.confirm_factory_reset)
    // sends the bare byte 0x33 and is wrong about it (its comment "the firmware has no token verification" does not match the firmware).
    Bytes b = {cmd::kFactoryReset};
    const std::string w = "reset";
    b.insert(b.end(), w.begin(), w.end());
    return b;
}

Bytes build_get_stats(uint8_t sub_type)
{
    return {cmd::kGetStats, sub_type};
}

Bytes build_set_other_params(bool manual_add, uint8_t telemetry_mode, uint8_t adv_loc_policy, uint8_t multi_acks, bool with_multi_acks)
{
    Bytes b = {cmd::kSetOtherParams, static_cast<uint8_t>(manual_add ? 1 : 0), telemetry_mode, adv_loc_policy};
    if (with_multi_acks) b.push_back(multi_acks);
    return b;
}

Bytes build_set_autoadd_config(uint8_t flags)
{
    return {cmd::kSetAutoadd, flags};
}

Bytes build_get_autoadd_config()
{
    return {cmd::kGetAutoadd};
}

Bytes build_get_allowed_repeat_freq()
{
    return {cmd::kGetAllowedRepeatFreq};
}

Bytes build_set_path_hash_mode(int mode)
{
    return {cmd::kSetPathHashMode, 0, static_cast<uint8_t>(mode)};
}

Bytes build_discover_request(uint8_t type_filter, uint32_t tag, bool prefix_only)
{
    Bytes b = {cmd::kSendControlData, static_cast<uint8_t>(0x80 | (prefix_only ? 1 : 0)), type_filter};
    put_u32(b, tag);
    return b;
}

/* ADVERT out of the radio log. Layout (packet_format.md, payloads.md): header (route type bits 0-1, payload type bits 2-5), 4 bytes of
 * transport codes for the two transport route types, the path length byte (hops in bits 0-5, hash size - 1 in bits 6-7), the path, then
 * the payload: public key (32), timestamp (4), signature (64), appdata (flags; latitude and longitude when 0x10; two 2-byte features when
 * 0x20 / 0x40; the name when 0x80). */
std::optional<HeardAdvert> parse_heard_advert(const LogData &log)
{
    const Bytes &p = log.raw;
    if (p.size() < 2) return std::nullopt;
    const uint8_t header = p[0];
    if (((header >> 2) & 0x0F) != 0x04) return std::nullopt;                // not PAYLOAD_TYPE_ADVERT
    size_t pos = 1;
    const int route = header & 0x03;
    if (route == 0 || route == 3) pos += 4;                                  // transport codes
    if (pos >= p.size()) return std::nullopt;
    const uint8_t plen = p[pos++];
    const int hash_size = (plen >> 6) + 1;
    if (hash_size > 3) return std::nullopt;                                  // 0b11 is reserved
    const int hops = plen & 0x3F;
    const size_t path_bytes = static_cast<size_t>(hops) * static_cast<size_t>(hash_size);
    if (pos + path_bytes > p.size()) return std::nullopt;
    pos += path_bytes;
    if (p.size() - pos < 32 + 4 + 64) return std::nullopt;
    HeardAdvert h;
    std::copy_n(p.begin() + static_cast<long>(pos), 32, h.key.begin());
    pos += 32;
    h.timestamp = static_cast<uint32_t>(p[pos]) | static_cast<uint32_t>(p[pos + 1]) << 8 | static_cast<uint32_t>(p[pos + 2]) << 16 |
                  static_cast<uint32_t>(p[pos + 3]) << 24;
    pos += 4 + 64;                                                           // timestamp, signature (not checked)
    h.hops = hops;
    h.hash_size = hash_size;
    h.snr = log.snr;
    h.rssi = log.rssi;
    if (pos < p.size()) {
        const uint8_t flags = p[pos++];
        const uint8_t type = flags & 0x0F;
        if (type >= advtype::kChat && type <= advtype::kSensor) h.type = type;
        auto i32 = [&](size_t at) {
            return static_cast<int32_t>(static_cast<uint32_t>(p[at]) | static_cast<uint32_t>(p[at + 1]) << 8 | static_cast<uint32_t>(p[at + 2]) << 16 |
                                        static_cast<uint32_t>(p[at + 3]) << 24);
        };
        if (flags & 0x10) {
            if (pos + 8 <= p.size()) {
                h.has_pos = true;
                h.lat = i32(pos) / 1e6;
                h.lon = i32(pos + 4) / 1e6;
                if (h.lat < -90.0 || h.lat > 90.0 || h.lon < -180.0 || h.lon > 180.0 || (h.lat == 0 && h.lon == 0)) h.has_pos = false;
            }
            pos += 8;
        }
        if (flags & 0x20) pos += 2;
        if (flags & 0x40) pos += 2;
        if ((flags & 0x80) && pos < p.size()) {
            std::string n(reinterpret_cast<const char *>(p.data() + pos), p.size() - pos);
            const size_t nul = n.find('\0');
            if (nul != std::string::npos) n.resize(nul);
            h.name = truncate_utf8(sanitize_utf8(n), 32);
        }
    }
    return h;
}

Bytes build_set_tx_power(int dbm)
{
    Bytes b = {cmd::kSetTxPower};
    put_u32(b, static_cast<uint32_t>(dbm));
    return b;
}

Bytes build_send_txt(const KeyPrefix &dst, const std::string &text, uint32_t timestamp, uint8_t attempt)
{
    Bytes b = {cmd::kSendTxt, txttype::kPlain, attempt};
    put_u32(b, timestamp);
    b.insert(b.end(), dst.begin(), dst.end());
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

Bytes build_send_channel_txt(uint8_t channel_idx, const std::string &text, uint32_t timestamp)
{
    Bytes b = {cmd::kSendChannelTxt, txttype::kPlain, channel_idx};
    put_u32(b, timestamp);
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

} // namespace meshzero
