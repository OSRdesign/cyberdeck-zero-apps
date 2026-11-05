/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the frame decoder of the Hi-Link LD2450-family tracking radars (LD2450, LD2452). No LVGL, no I/O, no
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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmw {

enum class Direction { Still, Approaching, Receding };

struct Target {
    int slot = 0;            /* 1..3 */
    bool present = false;    /* false: the 8 bytes of the slot were all zero */
    int x_mm = 0;            /* + to the right of the sensor */
    int y_mm = 0;            /* + in front of the sensor */
    int speed_cm_s = 0;      /* radial; negative = approaching */
    int resolution_mm = 0;
    int distance_mm = 0;     /* round(sqrt(x^2 + y^2)); 0 when absent */
    double angle_deg = 0.0;  /* atan2(x, y): 0 straight ahead, + to the right; 0 when absent */
    Direction direction = Direction::Still;
};

struct Frame {
    Target target[3];
    int present_count() const;
};

constexpr std::size_t kFrameSize = 30;

/* Decodes a 30-byte frame at p (header and footer must be right, else false). Always fills all three slots. */
bool decode_frame(const std::uint8_t *p, Frame &out);

/* Sign-magnitude 16-bit: bit 15 set = positive. */
int sign_magnitude(std::uint16_t raw);

/* Plausible values of a real radar (limits from the recorded walk with a wide margin): |x| <= 8000 mm,
 * 0 < y <= 8000 mm, |speed| <= 1000 cm/s, resolution <= 2000 mm; an empty slot is fine. Used to tell a radar from
 * noise that happens to carry the header bytes. */
bool plausible(const Frame &f);

struct DecoderStats {
    std::uint64_t bytes = 0;
    std::uint32_t frames_ok = 0;        /* good data frames yielded */
    std::uint32_t frames_bad = 0;       /* header seen, footer wrong: dropped, resynced on the next header */
    std::uint32_t frames_cut = 0;       /* data frame cut short by a command answer */
    std::uint32_t answers = 0;          /* command answers recognised and skipped */
    std::uint64_t garbage_bytes = 0;    /* bytes belonging to no frame */
    std::uint32_t good_run = 0;         /* consecutive plausible good frames (reset by anything else) */
    std::uint32_t resyncs() const { return frames_bad + frames_cut; }
};

class Decoder {
public:
    /* Feeds n bytes; appends every complete good frame to out (when not null). Returns the frames added. */
    std::size_t feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out);
    const DecoderStats &stats() const { return stats_; }
    void reset();

private:
    void process(std::vector<Frame> *out, std::size_t &added);
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    DecoderStats stats_;
};

} // namespace mmw
