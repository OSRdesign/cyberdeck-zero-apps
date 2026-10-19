/*
 * SPDX-License-Identifier: MIT
 *
 * Frames over Bluetooth LE (no LVGL, no D-Bus; unit tested).
 *
 * The companion firmware sends ONE frame per notification and takes ONE frame per write, without the serial wrapper
 * (docs/companion_protocol.md "Frame Handling"; SerialBLEInterface.cpp of the esp32 and nrf52 helpers). So:
 *   - a write must carry a whole frame: the transport never cuts a frame into several writes (the firmware would read each
 *     piece as a frame). A frame longer than MTU - 3 goes as ONE WriteValue all the same: BlueZ then makes it a long write
 *     (prepare + execute, src/gatt-client.c) and the board's GATT server puts it together.
 *   - a notification is a frame, except when the negotiated MTU is smaller than the firmware's frames: the nRF52 Bluefruit
 *     library then cuts one notify() into MTU - 3 pieces (BLECharacteristic::notify). NotifyAssembler joins those pieces
 *     back: a full piece (exactly MTU - 3 bytes) may go on, a shorter one ends the frame, and a full piece with nothing
 *     after it ends after hold_ms. With an MTU of 179 and up (chunk >= kBoardMaxFrame) no frame can be cut and every
 *     notification is passed through at once (BlueZ asks for 517; nRF52 boards give 247).
 *   - the Client keeps its serial parser: BleTransport wraps each received frame as '>' + u16 length + payload and takes
 *     the '<' + u16 length + payload frames the Client writes apart again.
 */

#pragma once

#include "ble_types.hpp"

#include <cstdint>
#include <vector>

namespace meshzero::ble {

/* '>' + u16 LE length + payload: a frame as the USB serial firmware would have sent it. */
Bytes wrap_from_device(const Bytes &payload);

/* Takes the Client's serial frames for the device ('<' + u16 LE length + payload, in any chunking) apart into payloads. */
class ToDeviceUnframer {
public:
    void feed(const uint8_t *data, size_t len, std::vector<Bytes> &out);
    void reset() { buf_.clear(); }
    size_t bad() const { return bad_; }
    size_t pending() const { return buf_.size(); }

private:
    Bytes buf_;
    size_t bad_ = 0;
};

/* How one frame of len bytes goes out with this MTU. */
enum class WritePlan {
    Single,     // fits one ATT Write Request (len <= MTU - 3)
    Long,       // longer than MTU - 3: still one WriteValue, BlueZ makes it a long write
    TooBig,     // empty or longer than the firmware takes (kBoardMaxFrame): refused
};
WritePlan plan_write(size_t len, int mtu);

/* Notifications -> frames (see the file comment). */
class NotifyAssembler {
public:
    explicit NotifyAssembler(int mtu = kDefaultMtu, uint64_t hold_ms = 50) : hold_ms_(hold_ms) { set_mtu(mtu); }

    void set_mtu(int mtu);
    int chunk() const { return chunk_; }
    bool reassembles() const { return chunk_ < static_cast<int>(kBoardMaxFrame); }

    /* One notification received at now_ms; complete frames are appended to out. */
    void feed(const uint8_t *data, size_t len, uint64_t now_ms, std::vector<Bytes> &out);
    /* Ends a held frame once hold_ms passed without a next piece. */
    void tick(uint64_t now_ms, std::vector<Bytes> &out);
    void reset() { pending_.clear(); }

    size_t joined() const { return joined_; }          // frames made of two pieces or more
    size_t held() const { return pending_.size(); }

private:
    void flush(std::vector<Bytes> &out);

    int chunk_ = kDefaultMtu - 3;
    uint64_t hold_ms_;
    Bytes pending_;
    size_t pieces_ = 0;
    uint64_t pending_since_ = 0;
    size_t joined_ = 0;
};

} // namespace meshzero::ble
