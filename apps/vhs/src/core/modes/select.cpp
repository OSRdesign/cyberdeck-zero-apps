/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: mode selection per board (see select.hpp).
 */
#include "select.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vhs {
namespace {

ModeReq mjpeg30(const Size &s)
{
    ModeReq m;
    m.fourcc = kMJPG;
    m.w = s.w;
    m.h = s.h;
    m.fps = 30;
    return m;
}

long area_diff(const Size &a, long area)
{
    return std::labs(a.area() - area);
}

} // namespace

const char *photo_mode_name(PhotoMode m)
{
    return m == PhotoMode::Light ? "light" : "instant";
}

bool parse_photo_mode(const std::string &s, PhotoMode &out)
{
    if (s == "instant") {
        out = PhotoMode::Instant;
        return true;
    }
    if (s == "light") {
        out = PhotoMode::Light;
        return true;
    }
    return false;
}

PhotoMode default_photo_mode(SocClass soc)
{
    switch (soc) {
    case SocClass::Bcm2712:
    case SocClass::Bcm2711:
    case SocClass::Bcm2837: return PhotoMode::Instant;
    case SocClass::Other: break;
    }
    return PhotoMode::Light;
}

std::string size_text(const Size &s)
{
    return std::to_string(s.w) + "x" + std::to_string(s.h);
}

bool parse_size(const std::string &text, Size &out)
{
    int w = 0, h = 0, n = 0;
    if (std::sscanf(text.c_str(), "%dx%d%n", &w, &h, &n) != 2 || static_cast<std::size_t>(n) != text.size()) return false;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    out.w = w;
    out.h = h;
    return true;
}

std::vector<Size> mjpeg30_sizes(const std::vector<CamMode> &listed)
{
    std::vector<Size> out;
    for (const CamMode &m : listed) {
        if (m.fourcc != kMJPG || !m.has_fps(30)) continue;
        Size s{m.w, m.h};
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const Size &a, const Size &b) { return a.area() > b.area(); });
    return out;
}

bool camera_portrait(const std::vector<CamMode> &listed)
{
    const CamMode *best = nullptr;
    for (const CamMode &m : listed)
        if (m.fourcc == kMJPG && (!best || static_cast<long>(m.w) * m.h > static_cast<long>(best->w) * best->h)) best = &m;
    if (!best)
        for (const CamMode &m : listed)
            if (!best || static_cast<long>(m.w) * m.h > static_cast<long>(best->w) * best->h) best = &m;
    return best && best->h > best->w;
}

Size remap_size(const std::vector<Size> &listed, const Size &stored, bool portrait)
{
    if (listed.empty()) return Size();
    if (!stored.valid()) return Size();
    for (const Size &s : listed)
        if (s == stored) return s;
    const Size t{stored.h, stored.w};
    for (const Size &s : listed)
        if (s == t) return s;
    const Size *best = nullptr;
    for (const Size &s : listed) {
        if (s.portrait() != portrait) continue;
        if (!best || area_diff(s, stored.area()) < area_diff(*best, stored.area())) best = &s;
    }
    if (!best)
        for (const Size &s : listed)
            if (!best || area_diff(s, stored.area()) < area_diff(*best, stored.area())) best = &s;
    return *best;
}

Selection select_modes(const SelectInput &in)
{
    Selection r;
    r.sizes = mjpeg30_sizes(in.listed);
    r.mode = in.have_photo_mode ? in.photo_mode : default_photo_mode(in.soc);
    r.mode_from_soc = !in.have_photo_mode;
    if (r.sizes.empty()) {
        /* no MJPEG at 30 fps: the YUYV live view (the largest YUYV 30 fps mode up to 1280 on the long side) */
        const CamMode *best = nullptr;
        for (const CamMode &m : in.listed) {
            if (m.fourcc != kYUYV || !m.has_fps(30) || std::max(m.w, m.h) > 1280) continue;
            if (!best || static_cast<long>(m.w) * m.h > static_cast<long>(best->w) * best->h) best = &m;
        }
        if (!best) return r;
        r.ok = true;
        r.yuyv_only = true;
        r.preview.fourcc = kYUYV;
        r.preview.w = best->w;
        r.preview.h = best->h;
        r.preview.fps = 30;
        return r;
    }
    r.ok = true;
    const bool cam_portrait = camera_portrait(in.listed);
    Size photo = remap_size(r.sizes, in.photo_size, cam_portrait);
    if (!photo.valid()) photo = r.sizes.front();
    r.photo = mjpeg30(photo);

    /* Light: nearest 1280x720 in the photo size's orientation, long side <= 1280 */
    const bool por = photo.portrait();
    const Size target = por ? Size{720, 1280} : Size{1280, 720};
    const Size *light = nullptr;
    for (const Size &s : r.sizes) {
        if (s.portrait() != por || std::max(s.w, s.h) > 1280) continue;
        if (!light || area_diff(s, target.area()) < area_diff(*light, target.area())) light = &s;
    }
    if (!light) { /* else the smallest mode that covers the box */
        for (const Size &s : r.sizes)
            if (s.w >= in.box_w && s.h >= in.box_h && (!light || s.area() < light->area())) light = &s;
    }
    r.light = mjpeg30(light ? *light : photo);
    r.light_is_instant = r.light.same(r.photo);
    r.preview = (r.mode == PhotoMode::Light && !r.light_is_instant) ? r.light : r.photo;

    Size video = remap_size(r.sizes, in.video_size, cam_portrait);
    if (!video.valid()) {
        const Size hd = cam_portrait ? Size{720, 1280} : Size{1280, 720};
        video = std::find(r.sizes.begin(), r.sizes.end(), hd) != r.sizes.end() ? hd : Size{r.light.w, r.light.h};
    }
    r.video = mjpeg30(video);
    return r;
}

std::string selection_text(const Selection &s, SocClass soc)
{
    if (!s.ok) return "modes: nothing to stream";
    if (s.yuyv_only) return "modes: no MJPEG at 30 fps: live view only, " + mode_text(s.preview);
    std::string t = "modes: photo " + mode_text(s.photo) + ", light " + mode_text(s.light) + ", video " +
                    mode_text(s.video) + ", preview " + mode_text(s.preview) + " (" + photo_mode_name(s.mode) +
                    (s.mode_from_soc ? std::string(" by soc ") + soc_name(soc) : std::string(" stored")) +
                    (s.light_is_instant ? ", light = photo size" : "") + ")";
    return t;
}

} // namespace vhs
