/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the sensor driver and frame decoder of the Hi-Link LD2450-family tracking radars (LD2450, LD2452). No LVGL, no I/O, no
 * globals. Written from the Hi-Link protocol documents and our own recordings (report 056); no third-party code.
 *
 * Tracking frame, 30 bytes, little-endian: AA FF 03 00 | 3 x target (8 bytes) | 55 CC. A target is x, y, speed and
 * resolution as 16-bit values. x, y and speed are sign-magnitude: bit 15 SET = positive, clear = negative, bits 0-14
 * the absolute value (not two's complement). Units in the core stay as the sensor sends them: x, y, resolution in
 * mm, speed in cm/s (units.hpp converts for the screen). x is positive to the right of the sensor, y positive in
 * front of it, a NEGATIVE speed is a target coming closer. A slot of 8 zero bytes is empty.
 *
 * Command answers (FD FC FB FA, length, data, 04 03 02 01) are recognised and skipped. A radar that answers a command
 * cuts the data frame it is sending short and the first byte of its answer arrives damaged (F8 or FC instead of FD):
 * "FC FB FA" never occurs inside a valid data frame, so the decoder resyncs on it. The app never sends a command; the
 * answers matter only for a sensor that another tool configures, and for the recorded fixture.
 *
 * The Decoder takes bytes in any chunking: feeding one byte at a time gives exactly the frames of one big feed.
 */
#pragma once

#include "driver.hpp"
#include "frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmw {

constexpr std::size_t kFrameSize = 30;

/* Decodes a 30-byte frame at p (header and footer must be right, else false). Always fills all three slots. */
bool decode_frame(const std::uint8_t *p, Frame &out);

/* Sign-magnitude 16-bit: bit 15 set = positive. */
int sign_magnitude(std::uint16_t raw);

/* Plausible values of a real radar (limits from the recorded walk with a wide margin): |x| <= 8000 mm,
 * 0 < y <= 8000 mm, |speed| <= 1000 cm/s, resolution <= 2000 mm; an empty slot is fine. Used to tell a radar from
 * noise that happens to carry the header bytes. */
bool plausible(const Frame &f);

class Decoder final : public FrameDecoder {
public:
    /* Feeds n bytes; appends every complete good frame to out (when not null). Returns the frames added. */
    std::size_t feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out) override;
    const DecoderStats &stats() const override { return stats_; }
    void reset() override;

private:
    void process(std::vector<Frame> *out, std::size_t &added);
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    DecoderStats stats_;
};

/* The driver of this family (registered in driver.cpp). */
extern const SensorDriver kDriverLd2450;

} // namespace mmw
