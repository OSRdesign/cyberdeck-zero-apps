/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: a scripted port layer for the headless scenes, the screenshots and a demonstration without the sensor
 * ($MMWAVE_DEMO). No device is touched: every port is made up here, and the "sensor" bytes are generated (or read from
 * the recorded walk) at the radar's own pace, 578 bytes (19.3 frames) a second, against the clock given.
 *
 * The made-up radar sends its frames at 9600 baud only (any other speed reads as zeros), as the owner's sensor does,
 * so the detector goes through its speed list as on the real port.
 *
 * Scenarios:
 *   walk    ttyUSB0 (CH340): the recorded walk of report 056 (tests/data/ld2452-9600-walk-15s.bin, or
 *           $MMWAVE_DEMO_FILE), looped, starting 3 s in
 *   one     ttyUSB0 (CH340): one person walking a slow loop (generated)
 *   three   ttyUSB0 (CH340): three people (generated)
 *   held    as three, but T2 is a stale target: byte-identical values in every frame (the LD2452's quirk)
 *   lost    as one, and the stream stops 1.5 s after the port was first opened
 *   none    ttyUSB0 (CP2102): a GPS receiver (NMEA); ttyACM0 (Pico): silent
 *   busy    ttyUSB0 (CH340): held by another app
 *   denied  ttyUSB0 (CH340): permission denied
 *   empty   no port at all
 * An unknown name is "empty".
 */
#pragma once

#include "frame.hpp"
#include "port_sys.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mmw {

/* The 30-byte tracking frame of f (the inverse of decode_frame; empty slots as 8 zero bytes). For the generated
 * scenarios and the tests: the app never sends anything to a port. */
void encode_frame(const Frame &f, std::uint8_t out[30]);

/* The generated people at time t_s (seconds): `count` targets (1 or 3) in the frame. */
Frame demo_frame(double t_s, int count);

class DemoSys : public Sys {
public:
    using Clock = std::function<std::int64_t()>;
    DemoSys(const std::string &scenario, Clock clock);

    std::vector<PortMeta> list_ports() override;
    bool held_by_other(const std::string &path) override;
    int open(const std::string &path, OpenErr &err) override;
    bool configure(int handle, int baud) override;
    void flush_input(int handle) override;
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void restore(int handle) override;
    void close(int handle) override;

    const std::string &scenario() const { return scenario_; }

private:
    enum class Kind { Walk, One, Three, Held, Gps, Silent };
    struct Port {
        PortMeta meta;
        Kind kind = Kind::Silent;
        bool held = false;
        bool denied = false;
        std::int64_t stop_after_ms = -1;   /* from the first open; -1 never */
        std::int64_t first_open_ms = -1;
        int handle = -1;
        int baud = 0;                      /* the speed set; the radar talks at 9600 only, as the owner's LD2452 */
        std::uint64_t pos = 0;             /* the next byte of the endless stream */
    };
    Port *by_path(const std::string &path);
    Port *by_handle(int handle);
    std::uint8_t byte_at(Port &p, std::uint64_t index);
    std::uint64_t stream_now(const Port &p, std::int64_t now) const;

    std::string scenario_;
    Clock clock_;
    std::int64_t t0_ = 0;
    std::vector<Port> ports_;
    std::vector<std::uint8_t> walk_;
    std::string nmea_;
    int next_handle_ = 100;
    std::uint64_t cached_frame_ = ~0ull;
    int cached_count_ = 0;
    std::uint8_t cache_[30] = {};
};

} // namespace mmw
