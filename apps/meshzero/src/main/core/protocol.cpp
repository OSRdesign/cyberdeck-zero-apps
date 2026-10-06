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
        }
        return l;
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

Bytes build_set_radio(double freq_mhz, double bw_khz, uint8_t sf, uint8_t cr)
{
    // meshcore_py truncates int(float(freq) * 1000); rounding avoids 869.618 becoming 869617 kHz.
    Bytes b = {cmd::kSetRadio};
    put_u32(b, static_cast<uint32_t>(std::llround(freq_mhz * 1000.0)));
    put_u32(b, static_cast<uint32_t>(std::llround(bw_khz * 1000.0)));
    b.push_back(sf);
    b.push_back(cr);
    return b;
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
