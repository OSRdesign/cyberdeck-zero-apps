/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: finds a USB GPS receiver and its position, on a background thread (the 0.1 intro screen's logic).
 *
 * Every ttyACM* and ttyUSB* of /dev is listened to (opened read-only, never written to) for NMEA sentences, at the
 * speed that worked last, then 9600, 4800, 38400 and 115200 baud, for 2.5 s / 1.5 s each; the first port that talks
 * NMEA is the receiver, a port with a fix wins. Then 4 s of rest and again (a receiver plugged in later is found; one
 * that gets its fix is seen within a few seconds). stop() ends the thread within about 200 ms.
 *
 * Tests: $VIZ_DEV_DIR replaces /dev; a regular file named ttyACM0 there is read like a port (to its end).
 */
#pragma once

#include "nmea.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace viz {

enum class GpsState { None, Waiting, Fix };

struct GpsSnapshot {
    GpsState state = GpsState::None;
    std::string port;   /* the device name: "ttyACM0" ("" with None) */
    std::string denied; /* None: a port that could not be opened (no permission), "" otherwise */
    double lat = 0, lon = 0;
    int sats = 0;
    std::uint64_t seq = 0; /* changes when anything above changed; 0 = no scan finished yet */
};

/* $VIZ_DEV_DIR, else /dev */
std::string serial_dev_dir();
/* ttyACM* and ttyUSB* under dir, sorted */
std::vector<std::string> serial_candidates(const std::string &dir);
/* Listens to path for up to ms milliseconds at baud (a regular file: read to its end). Stops early on a fix or when
 * stop becomes true. Never writes to the port. denied (optional): set when the port could not be opened for lack of
 * permission (EACCES / EPERM). */
NmeaResult listen_nmea(const std::string &path, int baud, int ms, const std::atomic<bool> &stop, bool *denied = nullptr);

class GpsWatcher {
public:
    explicit GpsWatcher(std::string dev_dir);
    ~GpsWatcher(); /* stop() */
    GpsWatcher(const GpsWatcher &) = delete;
    GpsWatcher &operator=(const GpsWatcher &) = delete;

    void start();
    void stop();
    GpsSnapshot snapshot() const;

private:
    void run();
    void publish(const NmeaResult &best, const std::string &port, const std::string &denied);

    std::string dev_dir_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    GpsSnapshot snap_;
};

} // namespace viz
