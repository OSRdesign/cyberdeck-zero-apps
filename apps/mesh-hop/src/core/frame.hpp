/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial framing of the MeshCore companion protocol.
 *
 * Written from the framing code of meshcore_py (serial_cx.py, MIT):
 *   device -> app : 0x3E ('>') | length (u16 little endian) | payload
 *   app -> device : 0x3C ('<') | length (u16 little endian) | payload
 * Bytes that are not part of a frame (boot messages, debug text printed on the same port) are skipped.
 */

#pragma once

#include "util.hpp"

#include <vector>

namespace meshzero {

constexpr uint8_t kFrameFromDevice = 0x3E;
constexpr uint8_t kFrameToDevice = 0x3C;
/* meshcore_py rejects received lengths above 300; the firmware's MAX_FRAME_SIZE is 172 (companion_protocol.md). */
constexpr size_t kMaxRxFrame = 300;
constexpr size_t kMaxTxFrame = 172;

/* Wraps a command payload for the device. Returns an empty vector when the payload is empty or too long. */
Bytes encode_frame(const Bytes &payload);

/* Incremental frame extractor: feed it whatever the port returned, in any chunking. */
class FrameParser {
public:
    /* Appends the complete frames found to out. */
    void feed(const uint8_t *data, size_t len, std::vector<Bytes> &out);
    void reset();

    size_t junk_bytes() const { return junk_; }    // bytes skipped outside any frame
    size_t bad_headers() const { return bad_; }    // 0x3E followed by an impossible length
    size_t pending() const { return buf_.size(); }

private:
    Bytes buf_;
    size_t junk_ = 0;
    size_t bad_ = 0;
};

} // namespace meshzero
