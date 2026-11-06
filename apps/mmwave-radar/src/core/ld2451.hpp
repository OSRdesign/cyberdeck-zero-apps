/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the sensor driver and frame decoder of the Hi-Link HLK-LD2451 (24 GHz, targets by distance, angle and
 * speed). No LVGL, no I/O, no globals. Written from Hi-Link's protocol document (V1.03) and our own recordings of the
 * sensor (tests/data/ld2451-*.bin); where the two differ the recordings win. No third-party code.
 *
 * Data frame, 115200 baud 8N1 (little-endian):
 *     F4 F3 F2 F1 | len (2) | count | alarm | count x {angle, distance, direction, speed, snr} | F8 F7 F6 F5
 * with len = 2 + 5 x count. A frame with NOTHING in view is F4 F3 F2 F1 00 00 F8 F7 F6 F5 (len 0: no count and no alarm
 * byte; about every 1.4 s); one with a target comes about every 0.14 s. A frame of len 2 (count 0) is accepted as
 * empty too. Per target, one byte each:
 *   angle      reported value - 0x80, degrees (1 degree steps). Which side is positive is NOT verified: the driver takes
 *              positive as to the right, seen from behind the sensor looking where it points (x = d sin a); the Flip
 *              angle setting mirrors it.
 *   distance   metres, whole (1 m steps; 2 m is the nearest seen; readings of 6-10 m in a 4 m room are reflections, not
 *              filtered here)
 *   direction  1 = approaching, 0 = receding (the document says the opposite; the recordings show the distance falling
 *              while the byte is 1)
 *   speed      km/h, a magnitude (signed by the direction in the Frame: approaching is NEGATIVE, as for the LD2450)
 *   snr        0..255
 * The alarm byte equalled the direction in every recording: checked for plausibility only, not used.
 *
 * Frame fields: x_mm = d sin(a), y_mm = d cos(a), distance_mm = d x 1000 and angle_deg = a (the wire values, kept
 * exact), resolution_mm 1000, speed_cm_s = +-round(km/h x 27.78), direction from the byte, snr from the wire.
 * Up to kLd2451MaxWireTargets (16) targets per frame are accepted; the first three fill the Frame's slots, the rest are
 * read and dropped. A count above that, a length that is not 0 or 2 + 5n, a count byte that does not match the length
 * or a wrong footer is a malformed frame: DecoderStats::frames_bad, resync on the next header.
 *
 * The app never sends anything: the document's configuration commands (including the one that would identify the
 * firmware) are not used, and this driver has no way to write.
 *
 * The decoder takes bytes in any chunking: feeding one byte at a time gives exactly the frames of one big feed. Bytes
 * that belong to no frame (the stray "F4 F3" seen in the captures) are skipped one at a time.
 */
#pragma once

#include "driver.hpp"
#include "frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmw {

constexpr std::size_t kLd2451MaxWireTargets = 16;

/* Decodes the n bytes at p (exactly one frame, header to footer) into out; false when it is not a valid LD2451 frame. */
bool ld2451_decode_frame(const std::uint8_t *p, std::size_t n, Frame &out);

/* A frame's total size from its first six bytes (header and length): 0 when the header or the length is wrong. */
std::size_t ld2451_frame_size(const std::uint8_t *p);

/* Plausible values of a real sensor: every angle within +-90 degrees, direction and alarm 0 or 1. Used to tell a radar
 * from noise that happens to carry the framing bytes. */
bool ld2451_plausible(const std::uint8_t *p, std::size_t n);

/* Polar to the Frame's x / y (mm): x = d sin(a), y = d cos(a). Exposed for the tests. */
void ld2451_polar_to_xy(int distance_m, int angle_deg, int &x_mm, int &y_mm);

class Ld2451Decoder final : public FrameDecoder {
public:
    std::size_t feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out) override;
    const DecoderStats &stats() const override { return stats_; }
    void reset() override;

private:
    void process(std::vector<Frame> *out, std::size_t &added);
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    DecoderStats stats_;
};

/* The driver of this sensor (registered in driver.cpp). */
extern const SensorDriver kDriverLd2451;

} // namespace mmw
