/*
 * SPDX-License-Identifier: MIT
 *
 * PUSH_LOG_RX_DATA (0x88): the board pushes every radio packet it receives: SNR*4 (int8), RSSI (int8), then the raw MeshCore
 * packet. This file decodes that frame (no LVGL, no I/O) and correlates the echoes of a channel message we sent.
 *
 * Raw packet: header byte (route type bits 0-1, payload type bits 2-5, version bits 6-7), 4 transport code bytes for the route
 * types 0 (TRANSPORT_FLOOD) and 3 (TRANSPORT_DIRECT), the path byte (low 6 bits: hop count, top 2 bits: hash size - 1), the path
 * (hop count * hash size bytes), then the payload up to the end. Checked against two frames from a real board (firmware v1.15.0).
 */

#pragma once

#include "util.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace meshzero {

namespace logdata {
constexpr uint8_t kPushLogRxData = 0x88;
constexpr uint8_t kRouteTransportFlood = 0, kRouteFlood = 1, kRouteDirect = 2, kRouteTransportDirect = 3;
constexpr uint8_t kPayloadReq = 0, kPayloadGrpTxt = 5;
constexpr size_t kMaxPathBytes = 64;       // MeshCore's MAX_PATH_SIZE
} // namespace logdata

struct LogPacket {
    double snr = 0;                        // dB, the board's value / 4
    int rssi = 0;                          // dBm
    uint8_t header = 0;
    uint8_t route_type = 0;                // logdata::kRoute*
    uint8_t payload_type = 0;              // 5 = GRP_TXT, 0 = REQ, ...
    uint8_t version = 0;
    bool has_transport = false;
    uint16_t transport[2] = {0, 0};        // little endian codes, only when has_transport
    uint8_t hop_count = 0;
    uint8_t hash_size = 1;                 // bytes per hop hash: 1..3
    Bytes path;                            // hop_count * hash_size bytes
    Bytes payload;
    Bytes raw;                             // the packet without the 3 byte push prefix

    bool is_group_text() const { return payload_type == logdata::kPayloadGrpTxt; }
    /* First payload byte of a GRP_TXT (the channel hash, 1 byte of the channel secret's SHA-256); -1 for other types or an empty payload. */
    int channel_hash() const { return is_group_text() && !payload.empty() ? payload[0] : -1; }
    /* Identity of the packet independent of the path: for a GRP_TXT the payload (hash + 2 byte MAC + ciphertext), repeaters only append
     * path hops and never change it. Empty for other types. Equal keys = the same transmission heard again (or a replay). */
    Bytes echo_key() const { return is_group_text() ? payload : Bytes(); }
};

/* Decodes a whole push frame (code byte included). False on anything that is not a well formed 0x88 frame: wrong code, too short,
 * path longer than the packet, hash size 4 (reserved), path over 64 bytes. Never reads out of bounds; out is only valid on true. */
bool parse_log_rx(const Bytes &frame, LogPacket &out);

/* Size of the payload of a GRP_TXT the app sends: channel hash (1) + MAC (2) + AES-128 ECB ciphertext, the plaintext
 * (timestamp 4 + flags 1 + "name: text" i.e. name_len + 2 + text_len) zero padded to a multiple of 16. Lengths in bytes (UTF-8). */
size_t expected_grp_txt_payload_size(size_t sender_name_len, size_t text_len);

/*
 * Correlates the packets we hear after sending a channel message. Honesty limits: the app cannot decrypt, so a match is "a GRP_TXT
 * of that channel hash heard within the window (and of the expected size, when given)": another sender on the same channel in the
 * same minute can be mistaken for an echo, the first such packet's payload is taken as ours. The channel hash is only 1 byte, so
 * unrelated channels collide 1 time in 256. heard_count() counts distinct paths of the same payload, not distinct repeaters: two
 * repeaters with equal hash prefixes look the same, a repeater heard over two different routes counts twice, and with a hop-hash of
 * 1 byte collisions are common. A packet heard with an empty path came straight from the sender (which is not us), so it is
 * counted too but says nothing about repeaters.
 */
class EchoTracker {
public:
    using Id = int;
    static constexpr Id kNone = -1;

    explicit EchoTracker(double window_s = 60.0) : window_(window_s) {}
    void set_window(double s) { window_ = s; }
    double window() const { return window_; }

    /* Starts watching for echoes of a message sent now (time in seconds, any monotonic clock). expected_payload_size 0 = any size.
     * Only the last 16 sends are kept. */
    Id register_sent(int channel_hash, double time, size_t expected_payload_size = 0);
    /* Feeds a heard packet. Returns the id of the send it belongs to, or kNone. The first matching packet fixes the payload of the
     * send; later packets count only when their payload is the same. A path already seen is not counted again. */
    Id on_packet(const LogPacket &pkt, double time);

    /* Distinct paths the payload was heard on (0 = nothing heard yet, or unknown id). */
    int heard_count(Id id) const;
    bool has_echo(Id id) const { return heard_count(id) > 0; }
    /* True once the window after the send has passed. */
    bool expired(Id id, double now) const;
    /* The payload (echo key) the send was matched to; empty before the first echo. */
    Bytes matched_key(Id id) const;

private:
    struct Sent {
        Id id = 0;
        int hash = 0;
        double time = 0;
        size_t size = 0;
        Bytes key;
        std::vector<Bytes> paths;          // hash size byte + path bytes
    };
    const Sent *find(Id id) const;
    double window_;
    Id next_ = 0;
    std::vector<Sent> sent_;
};

} // namespace meshzero
