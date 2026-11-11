/*
 * SPDX-License-Identifier: MIT
 */
#include "ring.hpp"

#include <algorithm>
#include <cstring>

namespace cassette {

Ring::Ring(const AudioFormat &fmt, double seconds) {
    fb_ = static_cast<size_t>(fmt.channels) * (fmt.bits / 8u);
    if (fb_ == 0) fb_ = 1;
    double f = seconds * fmt.rate;
    cap_ = f < 1.0 ? 1 : static_cast<size_t>(f);
    buf_.assign(cap_ * fb_, 0);
}

size_t Ring::write(const void *frames, size_t nframes) {
    const uint64_t w = w_.load(std::memory_order_relaxed);
    const uint64_t r = r_.load(std::memory_order_acquire);
    const size_t freef = cap_ - static_cast<size_t>(w - r);
    const size_t n = std::min(nframes, freef);
    if (n < nframes) dropped_.fetch_add(nframes - n, std::memory_order_relaxed);
    if (n == 0) return 0;
    const size_t pos = static_cast<size_t>(w % cap_);
    const size_t first = std::min(n, cap_ - pos);
    const unsigned char *src = static_cast<const unsigned char *>(frames);
    std::memcpy(&buf_[pos * fb_], src, first * fb_);
    if (n > first) std::memcpy(&buf_[0], src + first * fb_, (n - first) * fb_);
    w_.store(w + n, std::memory_order_release);
    return n;
}

size_t Ring::read(void *dst, size_t max_frames) {
    const uint64_t r = r_.load(std::memory_order_relaxed);
    const uint64_t w = w_.load(std::memory_order_acquire);
    const size_t n = std::min(max_frames, static_cast<size_t>(w - r));
    if (n == 0) return 0;
    const size_t pos = static_cast<size_t>(r % cap_);
    const size_t first = std::min(n, cap_ - pos);
    unsigned char *d = static_cast<unsigned char *>(dst);
    std::memcpy(d, &buf_[pos * fb_], first * fb_);
    if (n > first) std::memcpy(d + first * fb_, &buf_[0], (n - first) * fb_);
    r_.store(r + n, std::memory_order_release);
    return n;
}

size_t Ring::available() const {
    const uint64_t r = r_.load(std::memory_order_acquire);
    const uint64_t w = w_.load(std::memory_order_acquire);
    return static_cast<size_t>(w - r);
}

}  // namespace cassette
