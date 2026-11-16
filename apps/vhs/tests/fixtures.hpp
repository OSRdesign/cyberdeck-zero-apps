/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the cameras' mode lists (P0-b, plan 062 16.2 C2 and C14) the groups share.
 */
#pragma once

#include "camera/camera.hpp"

#include <vector>

namespace vhs {
namespace fx {

inline CamMode cm(std::uint32_t f, int w, int h, std::vector<int> fps)
{
    CamMode m;
    m.fourcc = f;
    m.w = w;
    m.h = h;
    m.fps = std::move(fps);
    return m;
}

/* the FineCam CM969, switch position B (landscape) */
inline std::vector<CamMode> finecam_b()
{
    return {cm(kMJPG, 1920, 1080, {60, 30}), cm(kMJPG, 1280, 960, {60, 30}), cm(kMJPG, 1280, 720, {60, 30}),
            cm(kMJPG, 1024, 576, {60, 30}),  cm(kMJPG, 800, 600, {60, 30}),  cm(kMJPG, 640, 480, {60, 30}),
            cm(kYUYV, 640, 480, {30}),       cm(kYUYV, 640, 360, {30})};
}

/* position A (portrait): MJPEG only, 30 fps */
inline std::vector<CamMode> finecam_a()
{
    return {cm(kMJPG, 1080, 1920, {30}), cm(kMJPG, 960, 1280, {30}), cm(kMJPG, 720, 1280, {30}),
            cm(kMJPG, 600, 800, {30}),   cm(kMJPG, 576, 1024, {30}), cm(kMJPG, 480, 640, {30})};
}

/* the Logitech C270: no 1080p, largest MJPEG 1280x960 */
inline std::vector<CamMode> c270()
{
    const std::vector<int> f = {30, 25, 20, 15, 10, 5};
    return {cm(kYUYV, 640, 480, f),       cm(kMJPG, 640, 480, f),  cm(kMJPG, 160, 120, f), cm(kMJPG, 320, 240, f),
            cm(kMJPG, 800, 600, f),       cm(kMJPG, 960, 720, f),  cm(kMJPG, 1024, 576, f), cm(kMJPG, 1280, 720, f),
            cm(kMJPG, 1280, 960, f),      cm(kYUYV, 1280, 960, {7, 5})};
}

/* a camera without MJPEG */
inline std::vector<CamMode> yuyv_only()
{
    return {cm(kYUYV, 640, 480, {30, 15}), cm(kYUYV, 1280, 720, {10}), cm(kYUYV, 320, 240, {30})};
}

} // namespace fx
} // namespace vhs
