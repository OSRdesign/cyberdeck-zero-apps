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
static_assert(kCidGain == V4L2_CID_GAIN, "control id");

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
    struct v4l2_queryctrl gq;
    if (query(api, fd, kCidGain, gq) && writable(gq) && gq.type == V4L2_CTRL_TYPE_INTEGER && gq.maximum > gq.minimum) {
        c.has_gain = true;
        c.gmin = gq.minimum;
        c.gmax = gq.maximum;
        c.gstep = gq.step > 0 ? gq.step : 1;
        c.gdef = std::max(gq.minimum, std::min(gq.maximum, gq.default_value));
        struct v4l2_control gg;
        std::memset(&gg, 0, sizeof gg);
        gg.id = kCidGain;
        if (api.ioctl(fd, VIDIOC_G_CTRL, &gg) == 0) c.gain_was = gg.value;
    }
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
    if (c.manual && c.has_gain)
        t += ", gain " + std::to_string(c.gmin) + ".." + std::to_string(c.gmax) + " (default " + std::to_string(c.gdef) + ")";
    t += c.autofocus ? ", continuous autofocus" : ", fixed focus";
    return t;
}

bool write_exposure(V4l2Api &api, int fd, const ExposureCaps &caps, bool manual, int value, int gain)
{
    if (!caps.manual) return false;
    if (manual) {
        if (!set_ctrl(api, fd, kCidExposureAuto, kExposureManualMode)) return false;
        if (!set_ctrl(api, fd, kCidExposureAbsolute, clamp_exposure(value, caps))) return false;
        if (gain >= 0 && caps.has_gain) set_ctrl(api, fd, kCidGain, clamp_gain(gain, caps)); /* a gain that is refused leaves the preview dimmer */
        return true;
    }
    const int back = caps.aperture ? kExposureAperturePriority : (caps.auto_was >= 0 && caps.auto_was != kExposureManualMode ? caps.auto_was : kExposureAperturePriority);
    if (caps.has_gain && caps.gain_was >= 0) set_ctrl(api, fd, kCidGain, caps.gain_was); /* best effort, while Manual is on (the gain is inactive in automatic modes) */
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

int clamp_gain(double value, const ExposureCaps &caps)
{
    const int step = caps.gstep > 0 ? caps.gstep : 1;
    double v = std::max<double>(caps.gmin, std::min<double>(caps.gmax, value));
    long r = static_cast<long>(caps.gmin) + std::lround((v - caps.gmin) / step) * step;
    while (r > caps.gmax) r -= step;
    return static_cast<int>(std::max<long>(caps.gmin, r));
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
    cap_s_ = caps.manual ? clamp_exposure(std::min(kPreviewShutterCap, caps.max), caps) : 0;
    stop_ = 0;
    seeking_ = at_limit_ = false;
    cur_ = photo_ = iter_ = 0;
    cur_gain_ = -1;
    base_ = target_ = 0;
    k_ = 0;
    have_prev_ = false;
    phase_ = Phase::Shutter;
    gain_used_ = false;
    pts_.clear();
}

ExposureMeter::Action ExposureMeter::issue(int shutter, int gain, int photo)
{
    Action a;
    a.kind = Kind::Manual;
    a.value = shutter;
    a.gain = gain;
    a.photo = photo;
    cur_ = shutter;
    cur_gain_ = gain;
    photo_ = photo;
    if (gain >= 0) gain_used_ = true;
    return a;
}

/* the shutter that gives the target at the default gain: read off the samples (the last two, in a straight line) */
int ExposureMeter::extrapolate() const
{
    if (pts_.empty()) return clamp_exposure(cap_s_, caps_);
    std::vector<std::pair<double, double>> p = pts_;
    std::sort(p.begin(), p.end());
    const auto &a = p.back();
    double x = a.first * target_ / std::max(1.0, a.second);
    for (std::size_t k = p.size() - 1; k > 0; --k) { /* the nearest lower sample whose luma differs */
        const auto &b = p[k - 1];
        if (a.second - b.second > 0.5 && a.first > b.first) {
            x = a.first + (target_ - a.second) / ((a.second - b.second) / (a.first - b.first));
            break;
        }
    }
    return clamp_exposure(std::max<double>(x, cap_s_), caps_);
}

/* the preview's next gain: the secant on the luma, else from a luma that grows in step with the gain's range */
int ExposureMeter::next_gain(double luma) const
{
    const double g = cur_gain_ >= 0 ? cur_gain_ : caps_.gdef;
    double next;
    if (have_prev_ && prev_x_ != g && luma - prev_luma_ > 0.3) {
        next = g + (target_ - luma) * (g - prev_x_) / (luma - prev_luma_);
    } else { /* assume the largest gain multiplies the picture by gmax / gdef */
        const double m = caps_.gdef > 0 ? static_cast<double>(caps_.gmax) / caps_.gdef : 2.0;
        const double ratio = target_ / std::max(1.0, luma);
        const double slope = std::max(0.01, (m - 1.0) / (caps_.gmax - caps_.gdef)); /* relative luma per gain step */
        next = g + (ratio - 1.0) / slope;
    }
    next = std::max<double>(g - (caps_.gmax - caps_.gmin) / 2.0, std::min<double>(g + (caps_.gmax - caps_.gmin) / 2.0, next));
    return clamp_gain(std::max<double>(caps_.gdef, next), caps_);
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
    phase_ = Phase::Shutter;
    if (stop == 0) {
        stop_ = 0;
        seeking_ = false;
        base_ = target_ = 0;
        a.kind = Kind::Auto;
        return a;
    }
    double start;
    if (stop_ == 0) { /* leaving Auto: L0 is measured now and kept; the picture of this scene is sampled again */
        base_ = auto_luma > 0 ? auto_luma : 110;
        pts_.clear();
        start = k_ > 0 ? target_luma(base_, stop) / k_ : caps_.def;
    } else {          /* stop to stop: the true shutter x 2^(difference) */
        start = std::max(photo_, 1) * stop_factor(stop - stop_);
    }
    stop_ = stop;
    target_ = target_luma(base_, stop);
    seeking_ = true;
    const int shutter = clamp_exposure(std::min<double>(std::max<double>(start, caps_.min), cap_s_), caps_);
    return issue(shutter, gain_used_ && caps_.has_gain ? caps_.gdef : -1, shutter);
}

ExposureMeter::Action ExposureMeter::sample(double luma)
{
    Action none;
    if (!seeking_ || stop_ == 0) return none;
    ++iter_;
    const bool close = luma_close(luma, target_);
    const bool limit_iter = iter_ >= kExposureMaxIterations;
    if (phase_ == Phase::Shutter) {
        if (luma > kLumaLow && luma < kLumaHigh && cur_ > 0) {
            k_ = luma / cur_;
            pts_.push_back({static_cast<double>(cur_), luma});
        }
        if (close) { /* the preview is the photo */
            seeking_ = false;
            if (photo_ == cur_) return none;
            return issue(cur_, cur_gain_, cur_);
        }
        const double l = std::max(1.0, luma);
        double next = cur_ * std::max(0.25, std::min(4.0, target_ / l));
        if (have_prev_ && prev_x_ != cur_ && std::fabs(luma - prev_luma_) > 0.5) { /* the secant, where the camera is linear */
            const double slope = (luma - prev_luma_) / (cur_ - prev_x_);
            if (slope > 0) next = std::max(cur_ / 4.0, std::min(cur_ * 4.0, cur_ + (target_ - luma) / slope));
        }
        prev_x_ = cur_;
        prev_luma_ = luma;
        have_prev_ = true;
        next = std::max<double>(caps_.min, std::min<double>(caps_.max, next));
        const int g = gain_used_ && caps_.has_gain ? caps_.gdef : -1;
        if (next <= cap_s_) { /* within the cap: the shutter alone */
            const int sh = clamp_exposure(next, caps_);
            if (sh == cur_ || limit_iter) {
                seeking_ = false;
                at_limit_ = sh == cur_ && !close; /* the control's end, or its step */
                if (photo_ == cur_) return none;
                return issue(cur_, cur_gain_, cur_);
            }
            return issue(sh, g, sh);
        }
        if (cur_ < cap_s_) { /* it wants more than the cap: the cap first (its luma starts the gain's loop) */
            if (limit_iter) {
                seeking_ = false;
                at_limit_ = true;
                return issue(cur_, cur_gain_, extrapolate());
            }
            return issue(cap_s_, g, photo_);
        }
        /* at the cap and still short of the target */
        if (caps_.has_gain && caps_.gmax > caps_.gdef && !limit_iter) {
            phase_ = Phase::Gain;
            have_prev_ = true; /* the cap at the default gain is the first point of the gain's secant */
            prev_x_ = caps_.gdef;
            prev_luma_ = luma;
            cur_gain_ = caps_.gdef;
            const int ng = next_gain(luma);
            if (ng == caps_.gdef) { /* the gain cannot help */
                seeking_ = false;
                at_limit_ = true;
                return issue(cap_s_, g, extrapolate());
            }
            return issue(cap_s_, ng, extrapolate());
        }
        seeking_ = false;
        at_limit_ = true; /* no gain to make it up: the preview is as bright as the cap allows */
        return issue(cur_, cur_gain_, extrapolate());
    }
    /* the gain's loop at the cap */
    if (close) {
        seeking_ = false;
        return issue(cur_, cur_gain_, extrapolate());
    }
    const int ng = next_gain(luma);
    prev_x_ = cur_gain_;
    prev_luma_ = luma;
    have_prev_ = true;
    if (ng == cur_gain_ || limit_iter) { /* the gain's end, or its step: as near as it gets */
        seeking_ = false;
        at_limit_ = true;
        return issue(cur_, cur_gain_, extrapolate());
    }
    return issue(cur_, ng, extrapolate());
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
