/*
 * SPDX-License-Identifier: MIT
 */

#include "logdata.hpp"

#include <algorithm>

namespace meshzero {

bool parse_log_rx(const Bytes &f, LogPacket &out)
{
    // code, snr, rssi, header, path byte: at least 5 bytes
    if (f.size() < 5 || f[0] != logdata::kPushLogRxData)
        return false;
    LogPacket p;
    p.snr = static_cast<int8_t>(f[1]) / 4.0;
    p.rssi = static_cast<int8_t>(f[2]);
    p.raw.assign(f.begin() + 3, f.end());
    const Bytes &r = p.raw;
    p.header = r[0];
    p.route_type = p.header & 3;
    p.payload_type = (p.header >> 2) & 0x0F;
    p.version = p.header >> 6;
    size_t pos = 1;
    if (p.route_type == logdata::kRouteTransportFlood || p.route_type == logdata::kRouteTransportDirect) {
        if (r.size() < pos + 4 + 1)
            return false;
        p.has_transport = true;
        p.transport[0] = static_cast<uint16_t>(r[pos] | (r[pos + 1] << 8));
        p.transport[1] = static_cast<uint16_t>(r[pos + 2] | (r[pos + 3] << 8));
        pos += 4;
    }
    const uint8_t plb = r[pos++];
    p.hop_count = plb & 63;
    const unsigned hsz = (plb >> 6) + 1u;
    if (hsz > 3)
        return false;
    p.hash_size = static_cast<uint8_t>(hsz);
    const size_t path_len = static_cast<size_t>(p.hop_count) * hsz;
    if (path_len > logdata::kMaxPathBytes || pos + path_len > r.size())
        return false;
    p.path.assign(r.begin() + pos, r.begin() + pos + path_len);
    p.payload.assign(r.begin() + pos + path_len, r.end());
    out = std::move(p);
    return true;
}

size_t expected_grp_txt_payload_size(size_t name_len, size_t text_len)
{
    const size_t plain = 4 + 1 + name_len + 2 + text_len;
    return 1 + 2 + (plain + 15) / 16 * 16;
}

EchoTracker::Id EchoTracker::register_sent(int channel_hash, double time, size_t expected_payload_size)
{
    Sent s;
    s.id = next_++;
    s.hash = channel_hash;
    s.time = time;
    s.size = expected_payload_size;
    sent_.push_back(std::move(s));
    if (sent_.size() > 16)
        sent_.erase(sent_.begin());
    return sent_.back().id;
}

EchoTracker::Id EchoTracker::on_packet(const LogPacket &pkt, double time)
{
    if (!pkt.is_group_text() || pkt.payload.empty())
        return kNone;
    auto in_window = [&](const Sent &s) { return pkt.payload[0] == s.hash && time >= s.time && time - s.time <= window_; };
    auto count = [&](Sent &s) {
        Bytes path;
        path.push_back(pkt.hash_size);
        path.insert(path.end(), pkt.path.begin(), pkt.path.end());
        if (std::find(s.paths.begin(), s.paths.end(), path) == s.paths.end())
            s.paths.push_back(std::move(path));
        return s.id;
    };
    // a payload already matched belongs to that send: a late copy of the first message must not become the key of a second one sent since
    for (Sent &s : sent_)
        if (!s.key.empty() && pkt.payload == s.key && in_window(s))
            return count(s);
    // newest send first: the most recent message is the likeliest owner of an echo
    for (auto it = sent_.rbegin(); it != sent_.rend(); ++it) {
        Sent &s = *it;
        if (!s.key.empty() || !in_window(s))
            continue;
        if (s.size && pkt.payload.size() != s.size)
            continue;
        s.key = pkt.payload;
        return count(s);
    }
    return kNone;
}

const EchoTracker::Sent *EchoTracker::find(Id id) const
{
    for (const Sent &s : sent_)
        if (s.id == id)
            return &s;
    return nullptr;
}

int EchoTracker::heard_count(Id id) const
{
    const Sent *s = find(id);
    return s ? static_cast<int>(s->paths.size()) : 0;
}

bool EchoTracker::expired(Id id, double now) const
{
    const Sent *s = find(id);
    return !s || now - s->time > window_;
}

Bytes EchoTracker::matched_key(Id id) const
{
    const Sent *s = find(id);
    return s ? s->key : Bytes();
}

} // namespace meshzero
