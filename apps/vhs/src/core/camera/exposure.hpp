/*
 * SPDX-License-Identifier: MIT
 *
 * VHS 0.1.2: exposure in stops, and the focus wait before a still. LVGL-free; the V4L2 side goes through V4l2Api (fake
 * camera in the tests).
 *
 * Exposure. The Capture page's Exposure row (Photo mode, cameras that can do it) offers Auto and -2, -1, +1, +2 stops
 * around what the camera's automatic exposure picks. UVC in Aperture Priority does not report the exposure the camera
 * chose (exposure_time_absolute reads back the last value written), so the stops are made in software:
 *   - Auto: auto_exposure = Aperture Priority (3), nothing else touched;
 *   - a stop N (not 0): the base luma L0 is the mean luma of the live picture at the moment the user leaves Auto (it is
 *     not measured again while a stop is on: the scene must not fight the user); auto_exposure goes to Manual (1) and
 *     exposure_time_absolute (units of 100 microseconds) is driven by a small feedback loop on the measured luma of the
 *     picture until it is within 8 % of L0 x 2^N (the target is kept in 8..245), at most 6 iterations. A change from one
 *     stop to another starts from the last value x 2^(difference).
 * The camera must offer auto_exposure with Manual Mode and an exposure_time_absolute that is writable; otherwise the row
 * is not shown and nothing is written (a camera without exposure controls behaves as before).
 *
 * Focus. A camera with continuous autofocus (focus_automatic_continuous present and writable) gets time to settle before a
 * still is taken: the frames' sizes (a JPEG's size follows its sharpness) must stop changing (FocusSettle), at most
 * kFocusCapMs after the first frame that would do for the photo.
 */
#pragma once

#include "v4l2_api.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace vhs {

constexpr std::uint32_t kCidExposureAuto = 0x009a0901;     /* V4L2_CID_EXPOSURE_AUTO */
constexpr std::uint32_t kCidExposureAbsolute = 0x009a0902; /* V4L2_CID_EXPOSURE_ABSOLUTE (exposure_time_absolute, 100 us) */
constexpr std::uint32_t kCidFocusAuto = 0x009a090c;        /* V4L2_CID_FOCUS_AUTO (focus_automatic_continuous) */
constexpr int kExposureManualMode = 1;                     /* V4L2_EXPOSURE_MANUAL */
constexpr int kExposureAperturePriority = 3;               /* V4L2_EXPOSURE_APERTURE_PRIORITY */

constexpr int kStopMin = -2, kStopMax = 2;
constexpr int kExposureMaxIterations = 6;
constexpr int kExposureSettleFrames = 5;   /* frames to let go by after a write before the luma counts */
constexpr double kLumaLow = 8, kLumaHigh = 245;
constexpr double kLumaTolerance = 0.08;
constexpr int kFocusCapMs = 1500;

struct ExposureCaps {
    bool manual = false;     /* auto_exposure has Manual Mode and exposure_time_absolute is writable: stops are possible */
    int min = 0, max = 0, step = 1, def = 0; /* exposure_time_absolute */
    int auto_was = -1;       /* auto_exposure as read at open (-1: unreadable) */
    bool aperture = false;   /* the menu has Aperture Priority (3) */
    bool autofocus = false;  /* focus_automatic_continuous exists and is writable */
    bool operator==(const ExposureCaps &o) const
    {
        return manual == o.manual && min == o.min && max == o.max && step == o.step && def == o.def && autofocus == o.autofocus;
    }
};

/* QUERYCTRL / QUERYMENU / G_CTRL at open. */
ExposureCaps detect_exposure_caps(V4l2Api &api, int fd);
/* "controls: exposure manual 50..10000 step 1 (default 166), autofocus" / "controls: no manual exposure, fixed focus". */
std::string exposure_caps_log(const ExposureCaps &c);

/* Writes the exposure: manual with `value` (clamped to the control's range and step), or automatic (Aperture Priority,
 * else the value read at open). Manual: auto_exposure first, then exposure_time_absolute. False when a write failed. */
bool write_exposure(V4l2Api &api, int fd, const ExposureCaps &caps, bool manual, int value);
/* Puts the automatic mode back (best effort), at close. */
void restore_auto_exposure(V4l2Api &api, int fd, const ExposureCaps &caps);

/* ------------------------------------------------------------------------------------------------- the maths (pure) */

int clamp_stop(int stop);
std::string stop_text(int stop);        /* 0 "Auto", 1 "+1", -2 "-2" */
double stop_factor(int stop);           /* 2^stop */
double target_luma(double base, int stop); /* base x 2^stop kept in kLumaLow..kLumaHigh */
bool luma_close(double luma, double target); /* within kLumaTolerance of the target (or 2 levels) */
/* A value for exposure_time_absolute: clamped to min..max, on the control's step grid. */
int clamp_exposure(double value, const ExposureCaps &caps);
/* The next value: cur x target / luma (the luma at cur), one step at most 4x either way, clamped. */
int next_exposure(int cur, double luma, double target, const ExposureCaps &caps);

/* The feedback loop (one per camera). The owner feeds it the user's choice and the luma of pictures taken after the last
 * write took effect; it answers with what to write. */
class ExposureMeter {
public:
    enum class Kind { None, Auto, Manual };
    struct Action {
        Kind kind = Kind::None;
        int value = 0; /* Manual: exposure_time_absolute */
    };
    void configure(const ExposureCaps &caps); /* a camera (re)opened: back to Auto, nothing remembered */
    void reset() { configure(caps_); }
    /* The user's choice; auto_luma: the luma of the live picture now (<= 0: unknown). */
    Action select(int stop, double auto_luma);
    /* A luma measured after the last write took effect. */
    Action sample(double luma);

    int stop() const { return stop_; }
    bool seeking() const { return seeking_; }
    bool at_limit() const { return at_limit_; }
    int value() const { return cur_; }      /* the last value commanded (0: none) */
    double base() const { return base_; }
    double target() const { return target_; }
    int iterations() const { return iter_; }
    void stop_seeking() { seeking_ = false; } /* gave up (a timeout): what is written stays */

private:
    ExposureCaps caps_;
    int stop_ = 0;
    bool seeking_ = false, at_limit_ = false;
    int cur_ = 0, iter_ = 0;
    double base_ = 0, target_ = 0;
    double k_ = 0; /* luma per unit of exposure, from the last unsaturated sample */
    bool have_prev_ = false; /* the sample before: a secant through the two last (value, luma) pairs speeds the search up */
    int prev_value_ = 0;
    double prev_luma_ = 0;
};

/* ------------------------------------------------------------------------------------------------- the focus wait */

/* Frame sizes in a window: settled when the last kWindow sizes differ by less than kTolerance of their mean. */
class FocusSettle {
public:
    static constexpr std::size_t kWindow = 6;
    static constexpr double kTolerance = 0.05;
    void reset() { sizes_.clear(); }
    void push(std::size_t bytes);
    bool settled() const;
    std::size_t count() const { return sizes_.size(); }

private:
    std::deque<std::size_t> sizes_;
};

} // namespace vhs
