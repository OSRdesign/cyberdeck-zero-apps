/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: mode selection per board (brief section 4). Pure, unit-tested; the capture thread calls it at every open with
 * the list the camera gives then (so a turned camera gets its portrait sizes).
 *
 *   - Only MJPEG modes the camera lists at 30 fps are offered; a mode that is not listed is never requested (C14).
 *   - A stored size that is not listed maps to its transposed size (1280x720 -> 720x1280 after a turn), then to the
 *     listed size nearest in area with the camera's orientation (that of its largest listed mode).
 *   - Photo size: the stored one (remapped), else the largest listed MJPEG mode.
 *   - Light mode: the listed MJPEG mode nearest 1280x720 (by area) in the Photo size's orientation with at most 1280 on
 *     the long side, else the smallest mode that covers the picture box.
 *   - Photo (Settings > Photo): the stored choice, else the SoC default: Instant on BCM2712, BCM2711, BCM2837 (Pi 5,
 *     CM5, Pi 4B, Pi 3A+, Zero 2 W), Light elsewhere (D6 ruling 2026-11-15). Light with Photo size == Light mode acts as
 *     Instant.
 *   - Preview: Instant streams the Photo-size mode; Light streams the Light mode.
 *   - Video size: the stored one (remapped), else 1280x720 MJPEG when listed, else the Light mode. P1 records 30 fps.
 *   - A camera without MJPEG: the YUYV live view only (largest YUYV mode at 30 fps with at most 1280 on the long side);
 *     photo and record are refused (section 20).
 */
#pragma once

#include "camera/camera.hpp"
#include "soc.hpp"

#include <string>
#include <vector>

namespace vhs {

enum class PhotoMode { Instant, Light };
const char *photo_mode_name(PhotoMode m); /* "instant", "light" */
bool parse_photo_mode(const std::string &s, PhotoMode &out);

PhotoMode default_photo_mode(SocClass soc);

struct Size {
    int w = 0, h = 0;
    bool valid() const { return w > 0 && h > 0; }
    bool operator==(const Size &o) const { return w == o.w && h == o.h; }
    bool operator!=(const Size &o) const { return !(*this == o); }
    long area() const { return static_cast<long>(w) * h; }
    bool portrait() const { return h > w; }
};
std::string size_text(const Size &s);                 /* "1920x1080" */
bool parse_size(const std::string &text, Size &out);  /* "1920x1080" */

/* The MJPEG sizes listed at 30 fps, largest area first. */
std::vector<Size> mjpeg30_sizes(const std::vector<CamMode> &listed);
/* The camera's orientation: that of its largest listed MJPEG (else any) mode. */
bool camera_portrait(const std::vector<CamMode> &listed);
/* A stored size on the sizes listed now: itself, else transposed, else the nearest in area with orientation
 * `portrait`. Invalid when the list is empty. */
Size remap_size(const std::vector<Size> &listed, const Size &stored, bool portrait);

struct SelectInput {
    std::vector<CamMode> listed;
    SocClass soc = SocClass::Other;
    bool have_photo_mode = false;   /* settings photo_mode present */
    PhotoMode photo_mode = PhotoMode::Instant;
    Size photo_size;                /* settings photo_size (invalid: none) */
    Size video_size;                /* settings video_size (invalid: none) */
    int box_w = 352, box_h = 198;   /* the picture box (Light's fallback) */
};

struct Selection {
    bool ok = false;          /* a mode to stream exists */
    bool yuyv_only = false;   /* no MJPEG: live view only */
    PhotoMode mode = PhotoMode::Instant; /* in force (stored or SoC default) */
    bool mode_from_soc = true;
    bool light_is_instant = false;       /* Light with Photo size == Light mode */
    ModeReq photo, light, video, preview;
    std::vector<Size> sizes;  /* the MJPEG 30 fps sizes offered (Settings later) */
};

Selection select_modes(const SelectInput &in);

/* One line for the log: "modes: photo MJPG 1920x1080 30 fps, light MJPG 1280x720 30 fps, video ..., preview ...
 * (instant, soc bcm2837)". */
std::string selection_text(const Selection &s, SocClass soc);

} // namespace vhs
