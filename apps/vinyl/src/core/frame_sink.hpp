/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 */
#pragma once
#include <cstddef>
#include <cstdint>
namespace vinyl {
struct AudioFormat { uint32_t rate = 48000; uint16_t channels = 1; uint16_t bits = 16; };
// Receives interleaved little-endian PCM frames from the capture thread. Must never block.
class FrameSink {
public:
  virtual ~FrameSink() = default;
  // Returns the number of frames accepted (fewer than offered when full: the caller counts the rest as dropped).
  virtual size_t write(const void* frames, size_t nframes) = 0;
};
}  // namespace vinyl
