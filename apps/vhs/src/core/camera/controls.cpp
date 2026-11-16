/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the exposure_auto_priority rule (see controls.hpp).
 */
#include "controls.hpp"

#include <linux/videodev2.h>

#include <cstring>

namespace vhs {

static_assert(kCidExposureAutoPriority == V4L2_CID_EXPOSURE_AUTO_PRIORITY, "control id");

ExposurePriority apply_exposure_priority(V4l2Api &api, int fd)
{
    ExposurePriority s;
    struct v4l2_queryctrl q;
    std::memset(&q, 0, sizeof q);
    q.id = kCidExposureAutoPriority;
    if (api.ioctl(fd, VIDIOC_QUERYCTRL, &q) != 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED)) return s;
    s.present = true;
    s.inactive = (q.flags & V4L2_CTRL_FLAG_INACTIVE) != 0;
    s.read_only = (q.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0;
    struct v4l2_control c;
    std::memset(&c, 0, sizeof c);
    c.id = kCidExposureAutoPriority;
    if (api.ioctl(fd, VIDIOC_G_CTRL, &c) == 0) s.was = c.value;
    if (!s.usable()) return s;
    std::memset(&c, 0, sizeof c);
    c.id = kCidExposureAutoPriority;
    c.value = 0;
    s.set = api.ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
    return s;
}

void restore_exposure_priority(V4l2Api &api, int fd, const ExposurePriority &s)
{
    if (!s.set || s.was < 0 || s.was == 0) return;
    struct v4l2_control c;
    std::memset(&c, 0, sizeof c);
    c.id = kCidExposureAutoPriority;
    c.value = s.was;
    api.ioctl(fd, VIDIOC_S_CTRL, &c); /* best effort */
}

std::string controls_log(const ExposurePriority &s)
{
    const std::string head = "controls: exposure_auto_priority ";
    if (!s.present) return head + "absent";
    const std::string was = std::to_string(s.was);
    if (s.read_only) return head + "read-only (" + was + ")";
    if (s.inactive) return head + "inactive (was " + was + ")";
    return head + "was " + was + (s.set ? ", set 0" : ", set failed");
}

} // namespace vhs
