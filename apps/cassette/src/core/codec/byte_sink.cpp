/* SPDX-License-Identifier: MIT */
#include "byte_sink.hpp"

#include <cerrno>
#include <cstring>

namespace cassette {

int FileByteSink::write(const void *p, size_t n) {
    const unsigned char *b = static_cast<const unsigned char *>(p);
    while (n > 0) {
        ssize_t w = ops_.pwrite(fd_, b, n, pos_);
        if (w == -EINTR) continue;
        if (w < 0) return static_cast<int>(w);
        if (w == 0) return -EIO;
        ++writes_;
        b += w;
        n -= static_cast<size_t>(w);
        pos_ += static_cast<uint64_t>(w);
        if (pos_ > end_) end_ = pos_;
    }
    return 0;
}

int FileByteSink::seek(uint64_t pos) {
    pos_ = pos;
    return 0;
}

int MemoryByteSink::write(const void *p, size_t n) {
    if (fail_at && pos_ + n > fail_at) return -fail_errno;
    if (pos_ + n > data.size()) data.resize(pos_ + n);
    if (n) std::memcpy(data.data() + pos_, p, n);
    pos_ += n;
    return 0;
}

int MemoryByteSink::seek(uint64_t pos) {
    ++seeks_;
    pos_ = pos;
    return 0;
}

}  // namespace cassette
