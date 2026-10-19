/*
 * SPDX-License-Identifier: MIT
 */

#include "ble_framing.hpp"

#include "../frame.hpp"

#include <algorithm>

namespace meshzero::ble {

Bytes wrap_from_device(const Bytes &payload)
{
    Bytes out;
    out.reserve(payload.size() + 3);
    out.push_back(kFrameFromDevice);
    out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void ToDeviceUnframer::feed(const uint8_t *data, size_t len, std::vector<Bytes> &out)
{
    buf_.insert(buf_.end(), data, data + len);
    size_t i = 0;
    while (i < buf_.size()) {
        if (buf_[i] != kFrameToDevice) {          // not the start of a frame: skip it
            ++i;
            ++bad_;
            continue;
        }
        if (buf_.size() - i < 3) break;           // the header is not complete yet
        const size_t n = static_cast<size_t>(buf_[i + 1]) | (static_cast<size_t>(buf_[i + 2]) << 8);
        if (n == 0 || n > kMaxRxFrame) {          // an impossible length: not a header
            ++i;
            ++bad_;
            continue;
        }
        if (buf_.size() - i - 3 < n) break;       // wait for the rest
        out.emplace_back(buf_.begin() + static_cast<long>(i + 3), buf_.begin() + static_cast<long>(i + 3 + n));
        i += 3 + n;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(i));
}

WritePlan plan_write(size_t len, int mtu)
{
    if (len == 0 || len > kBoardMaxFrame) return WritePlan::TooBig;
    const int room = std::max(mtu, kDefaultMtu) - 3;
    return len <= static_cast<size_t>(room) ? WritePlan::Single : WritePlan::Long;
}

void NotifyAssembler::set_mtu(int mtu)
{
    chunk_ = std::max(mtu, kDefaultMtu) - 3;
}

void NotifyAssembler::flush(std::vector<Bytes> &out)
{
    if (pending_.empty()) return;
    if (pieces_ > 1) ++joined_;
    out.push_back(std::move(pending_));
    pending_.clear();
    pieces_ = 0;
}

void NotifyAssembler::feed(const uint8_t *data, size_t len, uint64_t now_ms, std::vector<Bytes> &out)
{
    if (len == 0) return;
    if (!reassembles()) {                         // no frame can be cut at this MTU
        out.emplace_back(data, data + len);
        return;
    }
    // a piece that would make the frame longer than the firmware's longest cannot belong to the held one
    if (!pending_.empty() && pending_.size() + len > kBoardMaxFrame) flush(out);
    pending_.insert(pending_.end(), data, data + len);
    ++pieces_;
    pending_since_ = now_ms;
    if (len < static_cast<size_t>(chunk_) || pending_.size() >= kBoardMaxFrame) flush(out);   // a short piece ends it
}

void NotifyAssembler::tick(uint64_t now_ms, std::vector<Bytes> &out)
{
    if (!pending_.empty() && now_ms >= pending_since_ + hold_ms_) flush(out);
}

} // namespace meshzero::ble
