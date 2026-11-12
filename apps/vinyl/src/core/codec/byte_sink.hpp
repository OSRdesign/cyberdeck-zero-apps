/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette codecs: where an encoder's bytes go. The writer opens the take file through FileOps and hands the encoder a
 * FileByteSink over that descriptor, so the writer keeps the descriptor and calls fdatasync on its own timer exactly as
 * for WAV (the sink never syncs). The writes are positional (pwrite): FLAC seeks back once at the end to rewrite
 * STREAMINFO (total samples, MD5), everything else is appended. LVGL-free.
 */
#pragma once

#include "../fileops.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vinyl {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    /* Writes n bytes at the current position and moves past them: 0 or -errno (nothing is retried after an error). */
    virtual int write(const void *p, size_t n) = 0;
    /* Moves the position (FLAC's final STREAMINFO rewrite): 0 or -errno. */
    virtual int seek(uint64_t pos) = 0;
    virtual uint64_t tell() const = 0;
    /* The end of the data: the highest position ever written (the file length the encoder produced). */
    virtual uint64_t size() const = 0;
};

/* Over a descriptor the caller owns (opened, synced and closed by the caller). `start` is where the encoder's first byte
 * goes (0 for a fresh take file). */
class FileByteSink final : public ByteSink {
public:
    FileByteSink(FileOps &ops, int fd, uint64_t start = 0) : ops_(ops), fd_(fd), pos_(start), end_(start) {}
    int write(const void *p, size_t n) override;
    int seek(uint64_t pos) override;
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return end_; }
    uint64_t writes() const { return writes_; } /* pwrite calls that succeeded (tests) */

private:
    FileOps &ops_;
    int fd_;
    uint64_t pos_, end_;
    uint64_t writes_ = 0;
};

/* In memory: the tests, and an encoder run whose bytes are wanted as a buffer. `fail_at` makes every write that would
 * reach that position fail with `fail_errno` (a full card), 0 = never. */
class MemoryByteSink final : public ByteSink {
public:
    std::vector<unsigned char> data;
    uint64_t fail_at = 0;
    int fail_errno = 28; /* ENOSPC */
    int write(const void *p, size_t n) override;
    int seek(uint64_t pos) override;
    uint64_t tell() const override { return pos_; }
    uint64_t size() const override { return data.size(); }
    uint64_t seeks() const { return seeks_; }

private:
    uint64_t pos_ = 0;
    uint64_t seeks_ = 0;
};

}  // namespace vinyl
