/*
 * SPDX-License-Identifier: MIT
 */

#include "frame.hpp"

namespace meshzero {

Bytes encode_frame(const Bytes &payload)
{
    if (payload.empty() || payload.size() > kMaxTxFrame) return {};
    Bytes out;
    out.reserve(payload.size() + 3);
    out.push_back(kFrameToDevice);
    out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    out.push_back(static_cast<uint8_t>(payload.size() >> 8));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void FrameParser::reset()
{
    buf_.clear();
}

void FrameParser::feed(const uint8_t *data, size_t len, std::vector<Bytes> &out)
{
    buf_.insert(buf_.end(), data, data + len);
    size_t pos = 0;
    for (;;) {
        // find the start of a frame
        while (pos < buf_.size() && buf_[pos] != kFrameFromDevice) {
            ++pos;
            ++junk_;
        }
        if (buf_.size() - pos < 3) break;                      // header not complete yet
        const size_t size = static_cast<size_t>(buf_[pos + 1]) | (static_cast<size_t>(buf_[pos + 2]) << 8);
        if (size == 0 || size > kMaxRxFrame) {
            // not a real header (a '>' inside text): drop that one byte and look for the next marker. meshcore_py
            // drops the whole 3 byte header; skipping one byte also finds a real frame that starts inside it.
            ++pos;
            ++bad_;
            continue;
        }
        if (buf_.size() - pos < 3 + size) break;               // frame not complete yet
        out.emplace_back(buf_.begin() + static_cast<long>(pos + 3), buf_.begin() + static_cast<long>(pos + 3 + size));
        pos += 3 + size;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos));
}

} // namespace meshzero
