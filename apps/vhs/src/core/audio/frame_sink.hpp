/* Copied from Cassette (apps/cassette at origin/main fdad829) for VHS (brief 3.1, 5.4). */
#pragma once
#include <cstddef>
#include <cstdint>
namespace vhs {
struct AudioFormat { uint32_t rate = 48000; uint16_t channels = 1; uint16_t bits = 16; };
// Receives interleaved little-endian PCM frames from the capture thread. Must never block.
class FrameSink {
public:
  virtual ~FrameSink() = default;
  // Returns the number of frames accepted (fewer than offered when full: the caller counts the rest as dropped).
  virtual size_t write(const void* frames, size_t nframes) = 0;
};
}  // namespace vhs
