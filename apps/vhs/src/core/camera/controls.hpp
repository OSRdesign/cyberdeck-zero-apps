/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the one camera control P1 touches (brief 5.3, C14). V4L2_CID_EXPOSURE_AUTO_PRIORITY (v4l2-ctl names it
 * exposure_dynamic_framerate or exposure_auto_priority): when it exists, is writable and not inactive, its value is read
 * and it is set to 0 whenever a fixed 30 fps mode streams (every P1 mode), at every open (so again after an unplug, a
 * turn or a mode switch); the value read is put back at close, best effort. Without it a C270 drops itself to 15 fps
 * indoors. By control ID, never by name. No other control is touched in P1 (basic controls are P2). LVGL-free.
 */
#pragma once

#include "v4l2_api.hpp"

#include <cstdint>
#include <string>

namespace vhs {

constexpr std::uint32_t kCidExposureAutoPriority = 0x009a0903; /* V4L2_CID_EXPOSURE_AUTO_PRIORITY */

struct ExposurePriority {
    bool present = false;   /* QUERYCTRL knows it (and it is not disabled) */
    bool inactive = false;  /* V4L2_CTRL_FLAG_INACTIVE */
    bool read_only = false; /* V4L2_CTRL_FLAG_READ_ONLY */
    int was = -1;           /* the value read at open (-1 when it could not be read) */
    bool set = false;       /* set to 0 */
    bool usable() const { return present && !inactive && !read_only; }
};

/* At open, before REQBUFS: query it; when usable, read it and set it to 0. */
ExposurePriority apply_exposure_priority(V4l2Api &api, int fd);
/* At close: the value read at open goes back (best effort: the camera may be gone). */
void restore_exposure_priority(V4l2Api &api, int fd, const ExposurePriority &s);
/* The log line's text: "controls: exposure_auto_priority absent", "... was 1, set 0", "... inactive (was 1)",
 * "... read-only (1)", "... was 1, set failed". */
std::string controls_log(const ExposurePriority &s);

} // namespace vhs
