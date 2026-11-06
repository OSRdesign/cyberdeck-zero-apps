/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the sensor driver seam. One SensorDriver per sensor model (a const struct of data and function
 * pointers, no virtual dispatch on the driver itself): its names, the speeds to probe, a stateless `sniff` over a byte
 * window, a factory for the streaming FrameDecoder (frame.hpp) and what the UI may rely on. The detector, the link and
 * the settings know drivers only through this struct and the registry below.
 *
 * A driver without a decoder (make_decoder null) is a STUB: a model that is recognised on the wire and named in the
 * scan ("LD2410 presence radar (not supported yet)") but not read. Adding a model: see README, "Adding a driver".
 */
#pragma once

#include "frame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mmw {

/* What a driver says about a window of received bytes. Match: a complete valid frame of this model is in it. Maybe:
 * no complete frame yet, but the end of the window may be the start of one (wait for more bytes). No: nothing of this
 * model. */
enum class SniffResult { No, Maybe, Match };

/* The kind of view a driver's frames are shown in. Only Tracking exists today; Presence is for a later phase. */
enum class ViewKind { Tracking, Presence };

struct DriverCaps {
    bool tracking_targets;   /* frames carry up to three targets with positions */
    bool presence_gates;     /* frames carry presence and energy per range gate */
    bool polar = false;      /* the wire carries distance and angle (x and y are derived) */
};

struct SensorDriver {
    const char *id;            /* the settings file value: "ld2450" */
    const char *name;          /* the Sensor type label (short, it must fit at 320x480): "LD2452 / LD2450" */
    const char *model;         /* the Sensor page line: "LD2450-family tracking radar (LD2450, LD2452)" */
    const char *found_text;    /* the scan verdict for a port with this sensor: "mmWave radar (LD2450 family)" */
    const char *stub_text;     /* a stub's scan verdict: "LD2410 presence radar (not supported yet)"; "" for a real driver */
    const int *bauds;          /* the speeds to probe, in the detector's order */
    std::size_t baud_count;
    SniffResult (*sniff)(const std::uint8_t *data, std::size_t n);
    std::unique_ptr<FrameDecoder> (*make_decoder)(); /* null: a stub */
    DriverCaps caps;
    ViewKind view;
    /* Timing of the sensor, per model (the defaults are the LD2450 family's, which streams 19 frames a second):
     *   quiet_ms   no frame for this long = the sensor is lost (Link). An LD2451 with nothing in view sends one empty
     *              frame every 1.4 s (longest gap seen 2.8 s), so it needs more than the LD2450's 2 s.
     *   drop_ms    a tracker slot unseen for this long is released (Tracker). The LD2451 sends a target every 0.14 s
     *              but with gaps of up to 2.7 s inside one pass.
     *   listen_ms  0, or the listen window of the detector at the driver's first speed (bauds[0]) when it is longer than
     *              the detector's own (400 / 600 ms): three valid frames must fit in it.
     *   silent_ms  0, or how long a port may stay without a single byte at the first speed before it counts as silent
     *              (longer than the sensor's longest quiet interval).
     *   max_range_m the largest plot range the Auto range may grow to (view.hpp). */
    int quiet_ms = 2000;
    int drop_ms = 500;
    int listen_ms = 0;
    int silent_ms = 0;
    int max_range_m = 6;

    bool supported() const { return make_decoder != nullptr; }
};

/* The registry: a static array, real drivers first. Never null, never changes. */
const SensorDriver *const *drivers(std::size_t &count);
/* The driver with this id, or null. */
const SensorDriver *find_driver(const char *id);

/* The Sensor type setting values: "auto" or the id of a real driver. */
constexpr const char *kSensorAuto = "auto";
/* True for "auto" and for the id of a real (supported) driver. */
bool valid_sensor_type(const char *id);
/* The next value of the setting: auto -> the real drivers in registry order -> auto. */
const char *next_sensor_type(const char *id);
/* "Auto" or the driver's name. Unknown ids read as "Auto". */
const char *sensor_type_label(const char *id);

} // namespace mmw
