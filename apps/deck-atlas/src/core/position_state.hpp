/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: position.state, the file other apps read to know where the deck is (launcher report 054 section 6.3,
 * "position.state (draft)"). It lives in the launcher's state folder next to status.state ($APPLAUNCH_STATE_DIR, else
 * $XDG_RUNTIME_DIR/applaunch: a tmpfs, no SD writes), is written atomically (temporary file, fsync, rename) at most
 * once a second, and is read like the launcher's own state files: key=value lines, unknown keys ignored, an absent key
 * means unknown, and the file is believed only while its `pid` is alive.
 *
 *   v=1
 *   gen=812               grows with every write
 *   pid=2310              the writer (kill(pid, 0) says whether the file can be believed)
 *   writer=deck-atlas
 *   fix=3                 0 none, 2 2D, 3 3D
 *   lat=48.856613         degrees, south and west negative       (only with a fix)
 *   lon=2.352222
 *   alt_m=35.2            (3D only)
 *   speed_mps=1.40        (when known)
 *   course_deg=270.0      (when known: a moving fix)
 *   acc_m=7.5             estimated horizontal error (HDOP x 5, 3..100 m)
 *   hdop=1.50             (extra key of this writer)
 *   sats_used=8
 *   sats_view=11          (extra key of this writer)
 *   utc=2026-11-05T10:15:30Z   time of the fix, from the receiver
 *   mono_ms=123456789     CLOCK_MONOTONIC at the fix (age without trusting the wall clock); the write's time for fix=0
 *   source=usb:1546:01a8  or serial:ttyUSB0 (the receiver's USB id, else its device)
 *
 * No fix, a lost receiver or a stale fix (no valid sentence for 5 s) writes `fix=0` (v, gen, pid, writer, fix,
 * mono_ms, source only). Switching the GPS off, or leaving the app, removes the file.
 */
#pragma once

#include "gps_fix.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace atlas {

struct PositionStateInfo {
    int pid = 0;
    std::uint64_t gen = 0;
    std::string writer = "deck-atlas";
    std::string source;
    std::int64_t now_ms = 0;
};

/* The text of the file for a status (the full fix, or the `fix=0` form). */
std::string format_position_state(const GpsStatus &status, const PositionStateInfo &info);

/* "$APPLAUNCH_STATE_DIR", else "$XDG_RUNTIME_DIR/applaunch", else "" (no folder known: nothing is written). */
std::string position_state_dir();

class PositionStateWriter {
public:
    using WriteFn = std::function<bool(const std::string &path, const std::string &data)>;

    /* dir "": disabled. write: the atomic file write (atlas::write_file_atomic when empty). */
    PositionStateWriter(std::string dir, int pid, WriteFn write = nullptr);

    /* Looks at the status now: writes when its content changed and a second passed since the last write; Off removes
     * the file. Cheap to call every tick. */
    void update(const GpsStatus &status, const std::string &source, std::int64_t now_ms);
    /* Removes the file (the GPS went off, the app is leaving). */
    void clear();

    bool enabled() const { return !dir_.empty(); }
    const std::string &path() const { return path_; }
    int writes() const { return writes_; }

private:
    std::string dir_, path_;
    int pid_ = 0;
    WriteFn write_;
    std::uint64_t gen_ = 0;
    std::string last_sig_;
    std::int64_t last_write_ms_ = 0;
    bool wrote_ = false;   /* the file is there (we wrote it and did not remove it) */
    bool tried_ = false;   /* a write was tried at least once: last_write_ms_ is its time */
    int writes_ = 0;
};

} // namespace atlas
