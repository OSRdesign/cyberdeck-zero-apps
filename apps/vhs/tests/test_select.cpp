/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: mode selection per board (brief section 4): the FineCam in both switch positions, the C270 (no 1080p), a
 * YUYV-only camera; Instant / Light streams and the SoC defaults; the stored sizes remapped after a turn; the video
 * size; the SoC class from device tree compatible lists.
 */
#include "check.hpp"
#include "fixtures.hpp"

#include "modes/select.hpp"
#include "modes/soc.hpp"

using namespace vhs;

namespace {

SelectInput input(const std::vector<CamMode> &listed, SocClass soc)
{
    SelectInput in;
    in.listed = listed;
    in.soc = soc;
    return in;
}

void finecam()
{
    /* Instant on a Pi (any of the three classes): the preview streams the Photo size, 1080p */
    for (SocClass soc : {SocClass::Bcm2712, SocClass::Bcm2711, SocClass::Bcm2837}) {
        const Selection s = select_modes(input(fx::finecam_b(), soc));
        CHECK(s.ok);
        CHECK(!s.yuyv_only);
        CHECK(s.mode == PhotoMode::Instant);
        CHECK(s.mode_from_soc);
        CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1920x1080:30"));
        CHECK_EQ(mode_arg(s.light), std::string("MJPG:1280x720:30"));
        CHECK_EQ(mode_arg(s.video), std::string("MJPG:1280x720:30"));
        CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1920x1080:30"));
        CHECK(!s.light_is_instant);
    }
    /* elsewhere (the amd64 tablet): Light, the preview at 720p */
    Selection s = select_modes(input(fx::finecam_b(), SocClass::Other));
    CHECK(s.mode == PhotoMode::Light);
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1280x720:30"));
    /* a stored choice always wins */
    SelectInput in = input(fx::finecam_b(), SocClass::Bcm2837);
    in.have_photo_mode = true;
    in.photo_mode = PhotoMode::Light;
    s = select_modes(in);
    CHECK(s.mode == PhotoMode::Light);
    CHECK(!s.mode_from_soc);
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1280x720:30"));
    in = input(fx::finecam_b(), SocClass::Other);
    in.have_photo_mode = true;
    in.photo_mode = PhotoMode::Instant;
    CHECK_EQ(mode_arg(select_modes(in).preview), std::string("MJPG:1920x1080:30"));
    /* the sizes offered: the six MJPEG modes at 30 fps, largest first; no YUYV */
    CHECK_EQ(s.sizes.size(), static_cast<std::size_t>(6));
    if (s.sizes.size() == 6) {
        CHECK(s.sizes[0] == (Size{1920, 1080}));
        CHECK(s.sizes[5] == (Size{640, 480}));
    }
    /* position A (portrait): 1080x1920 photo, 720x1280 light and video */
    s = select_modes(input(fx::finecam_a(), SocClass::Bcm2711));
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1080x1920:30"));
    CHECK_EQ(mode_arg(s.light), std::string("MJPG:720x1280:30"));
    CHECK_EQ(mode_arg(s.video), std::string("MJPG:720x1280:30"));
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1080x1920:30"));
}

void c270()
{
    /* no 1080p: Photo size 1280x960 (the largest), the preview streams it on an Instant board */
    Selection s = select_modes(input(fx::c270(), SocClass::Bcm2711));
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1280x960:30"));
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1280x960:30"));
    CHECK_EQ(mode_arg(s.video), std::string("MJPG:1280x720:30"));
    bool any1080 = false;
    for (const Size &z : s.sizes) any1080 |= z.h == 1080;
    CHECK(!any1080);
    /* Light picks 1280x720 (nearest 1280x720, long side <= 1280) */
    CHECK_EQ(mode_arg(s.light), std::string("MJPG:1280x720:30"));
    /* with Photo size stored at 1280x720, Light equals it and acts as Instant */
    SelectInput in = input(fx::c270(), SocClass::Other);
    in.photo_size = Size{1280, 720};
    s = select_modes(in);
    CHECK(s.light_is_instant);
    CHECK(s.mode == PhotoMode::Light);
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1280x720:30"));
}

void yuyv_only()
{
    const Selection s = select_modes(input(fx::yuyv_only(), SocClass::Bcm2837));
    CHECK(s.ok);
    CHECK(s.yuyv_only);
    CHECK(s.sizes.empty());
    CHECK_EQ(mode_arg(s.preview), std::string("YUYV:640x480:30")); /* 1280x720 is 10 fps only */
    CHECK(!s.photo.valid());
    CHECK(!s.video.valid());
    CHECK(selection_text(s, SocClass::Bcm2837).find("live view only") != std::string::npos);
    const Selection none = select_modes(input({}, SocClass::Bcm2837));
    CHECK(!none.ok);
    CHECK(!none.preview.valid());
}

void remap()
{
    const std::vector<Size> b = mjpeg30_sizes(fx::finecam_b());
    const std::vector<Size> a = mjpeg30_sizes(fx::finecam_a());
    /* listed: itself */
    CHECK(remap_size(b, Size{1280, 720}, false) == (Size{1280, 720}));
    /* after a turn: the transposed size */
    CHECK(remap_size(a, Size{1280, 720}, true) == (Size{720, 1280}));
    CHECK(remap_size(a, Size{1920, 1080}, true) == (Size{1080, 1920}));
    CHECK(remap_size(b, Size{1080, 1920}, false) == (Size{1920, 1080}));
    /* not listed either way: the nearest in area with the camera's orientation (1280x960 turned is listed as 960x1280;
     * 1024x576 has no portrait twin: 576x1024 is listed; 800x600 -> 600x800) */
    CHECK(remap_size(a, Size{1280, 960}, true) == (Size{960, 1280}));
    CHECK(remap_size(a, Size{640, 360}, true) == (Size{480, 640}));
    CHECK(remap_size(b, Size{1366, 768}, false) == (Size{1280, 720}));
    CHECK(!remap_size({}, Size{1280, 720}, false).valid());
    CHECK(!remap_size(b, Size(), false).valid());
    /* stored sizes go through the remap in the selection: a stored 1920x1080 on the turned camera */
    SelectInput in = input(fx::finecam_a(), SocClass::Bcm2837);
    in.photo_size = Size{1920, 1080};
    in.video_size = Size{1280, 720};
    const Selection s = select_modes(in);
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1080x1920:30"));
    CHECK_EQ(mode_arg(s.video), std::string("MJPG:720x1280:30"));
    /* a stored size the C270 lacks (1080p): its nearest landscape size */
    in = input(fx::c270(), SocClass::Bcm2711);
    in.photo_size = Size{1920, 1080};
    CHECK_EQ(mode_arg(select_modes(in).photo), std::string("MJPG:1280x960:30"));
    CHECK(camera_portrait(fx::finecam_a()));
    CHECK(!camera_portrait(fx::finecam_b()));
    CHECK(!camera_portrait(fx::c270()));
    CHECK(!camera_portrait({}));
}

void light_fallback()
{
    /* a camera whose MJPEG modes are all over 1280 on the long side: Light is the smallest that covers the box */
    const std::vector<CamMode> big = {fx::cm(kMJPG, 2560, 1440, {30}), fx::cm(kMJPG, 1920, 1080, {30}),
                                      fx::cm(kMJPG, 3840, 2160, {30})};
    SelectInput in = input(big, SocClass::Other);
    in.box_w = 352;
    in.box_h = 198;
    const Selection s = select_modes(in);
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:3840x2160:30"));
    CHECK_EQ(mode_arg(s.light), std::string("MJPG:1920x1080:30"));
    CHECK_EQ(mode_arg(s.video), std::string("MJPG:1920x1080:30")); /* no 1280x720: the Light mode */
    /* MJPEG at 15 fps only is not offered */
    const std::vector<CamMode> slow = {fx::cm(kMJPG, 1920, 1080, {15}), fx::cm(kYUYV, 640, 480, {30})};
    const Selection t = select_modes(input(slow, SocClass::Bcm2837));
    CHECK(t.yuyv_only);
}

void texts()
{
    Size z;
    CHECK(parse_size("1920x1080", z));
    CHECK(z == (Size{1920, 1080}));
    CHECK_EQ(size_text(z), std::string("1920x1080"));
    CHECK(!parse_size("1920", z));
    CHECK(!parse_size("0x10", z));
    CHECK(!parse_size("1920x1080 ", z));
    PhotoMode m = PhotoMode::Instant;
    CHECK(parse_photo_mode("light", m));
    CHECK(m == PhotoMode::Light);
    CHECK(!parse_photo_mode("quick", m));
    CHECK_EQ(std::string(photo_mode_name(PhotoMode::Instant)), std::string("instant"));
    const Selection s = select_modes(input(fx::finecam_b(), SocClass::Bcm2837));
    const std::string t = selection_text(s, SocClass::Bcm2837);
    CHECK(t.find("preview MJPG 1920x1080 30 fps") != std::string::npos);
    CHECK(t.find("instant by soc bcm2837") != std::string::npos);
}

void soc()
{
    const std::string pi5("raspberrypi,5-model-b\0brcm,bcm2712\0", 35);
    CHECK(soc_from_compatible(pi5) == SocClass::Bcm2712);
    CHECK(soc_from_compatible(std::string("raspberrypi,4-model-b\0brcm,bcm2711\0", 35)) == SocClass::Bcm2711);
    CHECK(soc_from_compatible(std::string("raspberrypi,3-model-a-plus\0brcm,bcm2837\0", 40)) == SocClass::Bcm2837);
    CHECK(soc_from_compatible(std::string("raspberrypi,model-zero-2-w\0brcm,bcm2837\0", 40)) == SocClass::Bcm2837);
    CHECK(soc_from_compatible("") == SocClass::Other);
    CHECK(soc_from_compatible("brcm,bcm2835") == SocClass::Other);
    CHECK(default_photo_mode(SocClass::Bcm2837) == PhotoMode::Instant);
    CHECK(default_photo_mode(SocClass::Other) == PhotoMode::Light);
    const std::string root = make_temp_dir("soc");
    CHECK(detect_soc(root) == SocClass::Other); /* a PC: no device tree */
    make_dirs(root + "/proc/device-tree");
    write_file(root + "/proc/device-tree/compatible", std::string("raspberrypi,model-zero-2-w\0brcm,bcm2837\0", 40));
    CHECK(detect_soc(root) == SocClass::Bcm2837);
    CHECK_EQ(std::string(soc_name(SocClass::Bcm2712)), std::string("bcm2712"));
    rm_tree(root);
}

} // namespace

void test_select()
{
    finecam();
    c270();
    yuyv_only();
    remap();
    light_fallback();
    texts();
    soc();
}
