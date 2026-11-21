/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: exposure in stops and the focus wait (see exposure.hpp).
 */
#include "exposure.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vhs {

static_assert(kCidExposureAuto == V4L2_CID_EXPOSURE_AUTO, "control id");
static_assert(kCidExposureAbsolute == V4L2_CID_EXPOSURE_ABSOLUTE, "control id");
static_assert(kCidFocusAuto == V4L2_CID_FOCUS_AUTO, "control id");

namespace {

bool query(V4l2Api &api, int fd, std::uint32_t id, struct v4l2_queryctrl &q)
{
    std::memset(&q, 0, sizeof q);
    q.id = id;
    return api.ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0 && !(q.flags & V4L2_CTRL_FLAG_DISABLED);
}

bool writable(const struct v4l2_queryctrl &q)
{
    return !(q.flags & V4L2_CTRL_FLAG_READ_ONLY);
}

bool menu_has(V4l2Api &api, int fd, std::uint32_t id, int index)
{
    struct v4l2_querymenu m;
    std::memset(&m, 0, sizeof m);
    m.id = id;
    m.index = static_cast<std::uint32_t>(index);
    return api.ioctl(fd, VIDIOC_QUERYMENU, &m) == 0;
}

bool set_ctrl(V4l2Api &api, int fd, std::uint32_t id, int value)
{
    struct v4l2_control c;
    std::memset(&c, 0, sizeof c);
    c.id = id;
    c.value = value;
    return api.ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

} // namespace

ExposureCaps detect_exposure_caps(V4l2Api &api, int fd)
{
    ExposureCaps c;
    struct v4l2_queryctrl q;
    if (query(api, fd, kCidFocusAuto, q) && writable(q)) c.autofocus = true;
    struct v4l2_queryctrl am;
    if (!query(api, fd, kCidExposureAuto, am) || am.type != V4L2_CTRL_TYPE_MENU || !writable(am)) return c;
    struct v4l2_queryctrl ab;
    if (!query(api, fd, kCidExposureAbsolute, ab) || !writable(ab)) return c;
    if (!menu_has(api, fd, kCidExposureAuto, kExposureManualMode)) return c;
    c.aperture = menu_has(api, fd, kCidExposureAuto, kExposureAperturePriority);
    struct v4l2_control g;
    std::memset(&g, 0, sizeof g);
    g.id = kCidExposureAuto;
    if (api.ioctl(fd, VIDIOC_G_CTRL, &g) == 0) c.auto_was = g.value;
    c.min = ab.minimum;
    c.max = ab.maximum;
    c.step = ab.step > 0 ? ab.step : 1;
    c.def = ab.default_value;
    if (c.max < c.min) return c;
    c.manual = true;
    return c;
}

std::string exposure_caps_log(const ExposureCaps &c)
{
    std::string t = "controls: ";
    if (c.manual)
        t += "manual exposure " + std::to_string(c.min) + ".." + std::to_string(c.max) + " step " + std::to_string(c.step) +
             " (default " + std::to_string(c.def) + ", auto_exposure was " + std::to_string(c.auto_was) + ")";
    else t += "no manual exposure";
    t += c.autofocus ? ", continuous autofocus" : ", fixed focus";
    return t;
}

bool write_exposure(V4l2Api &api, int fd, const ExposureCaps &caps, bool manual, int value)
{
    if (!caps.manual) return false;
    if (manual) {
        if (!set_ctrl(api, fd, kCidExposureAuto, kExposureManualMode)) return false;
        return set_ctrl(api, fd, kCidExposureAbsolute, clamp_exposure(value, caps));
    }
    const int back = caps.aperture ? kExposureAperturePriority : (caps.auto_was >= 0 && caps.auto_was != kExposureManualMode ? caps.auto_was : kExposureAperturePriority);
    return set_ctrl(api, fd, kCidExposureAuto, back);
}

void restore_auto_exposure(V4l2Api &api, int fd, const ExposureCaps &caps)
{
    write_exposure(api, fd, caps, false, 0); /* best effort: the camera may be gone */
}

/* ------------------------------------------------------------------------------------------------- the maths */

int clamp_stop(int stop)
{
    return std::max(kStopMin, std::min(kStopMax, stop));
}

std::string stop_text(int stop)
{
    if (stop == 0) return "Auto";
    return (stop > 0 ? "+" : "-") + std::to_string(std::abs(stop));
}

double stop_factor(int stop)
{
    return std::ldexp(1.0, stop);
}

double target_luma(double base, int stop)
{
    return std::max(kLumaLow, std::min(kLumaHigh, base * stop_factor(stop)));
}

bool luma_close(double luma, double target)
{
    return std::fabs(luma - target) <= std::max(2.0, kLumaTolerance * target);
}

int clamp_exposure(double value, const ExposureCaps &caps)
{
    const int step = caps.step > 0 ? caps.step : 1;
    const double lo = caps.min, hi = caps.max;
    double v = std::max(lo, std::min(hi, value));
    long k = std::lround((v - lo) / step);
    long r = static_cast<long>(caps.min) + k * step;
    while (r > caps.max) r -= step; /* the last grid point not above max */
    return static_cast<int>(std::max<long>(caps.min, r));
}

int next_exposure(int cur, double luma, double target, const ExposureCaps &caps)
{
    const double l = std::max(1.0, luma);
    double ratio = target / l;
    ratio = std::max(0.25, std::min(4.0, ratio));
    return clamp_exposure(static_cast<double>(std::max(cur, 1)) * ratio, caps);
}

/* ------------------------------------------------------------------------------------------------- the meter */

void ExposureMeter::configure(const ExposureCaps &caps)
{
    caps_ = caps;
    stop_ = 0;
    seeking_ = at_limit_ = false;
    cur_ = iter_ = 0;
    base_ = target_ = 0;
    k_ = 0;
    have_prev_ = false;
}

ExposureMeter::Action ExposureMeter::select(int stop, double auto_luma)
{
    stop = clamp_stop(stop);
    Action a;
    if (!caps_.manual) return a;
    if (stop == stop_) return a;
    at_limit_ = false;
    iter_ = 0;
    have_prev_ = false;
    if (stop == 0) {
        stop_ = 0;
        seeking_ = false;
        base_ = target_ = 0;
        a.kind = Kind::Auto;
        return a;
    }
    int start;
    if (stop_ == 0) { /* leaving Auto: L0 is measured now and kept */
        base_ = auto_luma > 0 ? auto_luma : 110;
        start = k_ > 0 ? clamp_exposure(target_luma(base_, stop) / k_, caps_) : clamp_exposure(caps_.def, caps_);
    } else {          /* stop to stop: the last value, a factor 2 per stop */
        start = clamp_exposure(std::max(cur_, 1) * stop_factor(stop - stop_), caps_);
    }
    stop_ = stop;
    target_ = target_luma(base_, stop);
    cur_ = start;
    seeking_ = true;
    a.kind = Kind::Manual;
    a.value = start;
    return a;
}

ExposureMeter::Action ExposureMeter::sample(double luma)
{
    Action a;
    if (!seeking_ || stop_ == 0) return a;
    ++iter_;
    if (luma > kLumaLow && luma < kLumaHigh && cur_ > 0) k_ = luma / cur_;
    if (luma_close(luma, target_)) {
        seeking_ = false;
        return a;
    }
    int next = next_exposure(cur_, luma, target_, caps_);
    if (have_prev_ && prev_value_ != cur_ && std::fabs(luma - prev_luma_) > 0.5) { /* the secant, where the camera is linear */
        const double slope = (luma - prev_luma_) / static_cast<double>(cur_ - prev_value_);
        if (slope > 0) {
            const double cand = cur_ + (target_ - luma) / slope;
            const int lo = clamp_exposure(cur_ / 4.0, caps_), hi = clamp_exposure(cur_ * 4.0, caps_);
            next = std::max(lo, std::min(hi, clamp_exposure(cand, caps_)));
        }
    }
    prev_value_ = cur_;
    prev_luma_ = luma;
    have_prev_ = true;
    if (next == cur_) { /* the control's end (or too fine a step): this is as near as it gets */
        seeking_ = false;
        at_limit_ = true;
        return a;
    }
    if (iter_ >= kExposureMaxIterations) {
        seeking_ = false;
        return a;
    }
    cur_ = next;
    a.kind = Kind::Manual;
    a.value = next;
    return a;
}

/* ------------------------------------------------------------------------------------------------- the focus wait */

void FocusSettle::push(std::size_t bytes)
{
    if (bytes == 0) return;
    sizes_.push_back(bytes);
    while (sizes_.size() > kWindow) sizes_.pop_front();
}

bool FocusSettle::settled() const
{
    if (sizes_.size() < kWindow) return false;
    std::size_t lo = sizes_.front(), hi = sizes_.front(), sum = 0;
    for (std::size_t s : sizes_) {
        lo = std::min(lo, s);
        hi = std::max(hi, s);
        sum += s;
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(sizes_.size());
    return static_cast<double>(hi - lo) <= kTolerance * mean;
}

} // namespace vhs
