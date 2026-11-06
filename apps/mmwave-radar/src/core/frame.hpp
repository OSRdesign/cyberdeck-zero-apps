/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the common frame model every sensor driver (driver.hpp) decodes into, and the streaming decoder
 * interface. No LVGL, no I/O, no globals.
 *
 * A Frame holds up to three targets (slots 1..3). Units stay as the sensor sends them: x, y, resolution and distance in
 * mm, speed in cm/s (units.hpp converts for the screen); x is positive to the right of the sensor, y in front of it, a
 * NEGATIVE speed is a target coming closer. A driver fills x_mm and y_mm; distance_mm, angle_deg and direction are
 * derived from them by the LD2450 driver, and a polar sensor (distance and angle on the wire) fills them from the wire
 * and derives x and y. snr is optional (-1: the sensor sends none).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmw {

enum class Direction { Still, Approaching, Receding };

struct Target {
    int slot = 0;            /* 1..3 */
    bool present = false;    /* false: the slot is empty */
    int x_mm = 0;            /* + to the right of the sensor */
    int y_mm = 0;            /* + in front of the sensor */
    int speed_cm_s = 0;      /* radial; negative = approaching */
    int resolution_mm = 0;
    int distance_mm = 0;     /* round(sqrt(x^2 + y^2)); 0 when absent */
    double angle_deg = 0.0;  /* atan2(x, y): 0 straight ahead, + to the right; 0 when absent */
    Direction direction = Direction::Still;
    int snr = -1;            /* signal-to-noise, 0..255, when the sensor reports it; -1 unset */
};

struct Frame {
    Target target[3];
    std::uint8_t raw[30] = {}; /* the frame's bytes as received (for the diagnostic log; the first 30 for a longer one) */
    int present_count() const
    {
        int n = 0;
        for (const Target &t : target) n += t.present ? 1 : 0;
        return n;
    }
};

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

/* A streaming decoder: bytes in any chunking, frames out (feeding one byte at a time gives exactly the frames of one
 * big feed). One per open port, made by SensorDriver::make_decoder. */
class FrameDecoder {
public:
    virtual ~FrameDecoder() {}
    /* Feeds n bytes; appends every complete good frame to out (when not null). Returns the frames added. */
    virtual std::size_t feed(const std::uint8_t *data, std::size_t n, std::vector<Frame> *out) = 0;
    virtual const DecoderStats &stats() const = 0;
    virtual void reset() = 0;
};

} // namespace mmw
