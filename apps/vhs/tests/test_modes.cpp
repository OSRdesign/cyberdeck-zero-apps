/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: camera facts (mode text and parse, resolve against the FineCam's lists, the list helpers).
 */
#include "check.hpp"

#include "camera/camera.hpp"

using namespace vhs;

namespace {

CamMode cm(std::uint32_t f, int w, int h, std::vector<int> fps)
{
    CamMode m;
    m.fourcc = f;
    m.w = w;
    m.h = h;
    m.fps = std::move(fps);
    return m;
}

/* the CM969's lists (plan 062, 16.2 C2) */
std::vector<CamMode> position_b()
{
    return {cm(kMJPG, 1920, 1080, {60, 30}), cm(kMJPG, 1280, 960, {60, 30}), cm(kMJPG, 1280, 720, {60, 30}),
            cm(kMJPG, 1024, 576, {60, 30}),  cm(kMJPG, 800, 600, {60, 30}),  cm(kMJPG, 640, 480, {60, 30}),
            cm(kYUYV, 640, 480, {30}),       cm(kYUYV, 640, 360, {30})};
}

std::vector<CamMode> position_a()
{
    return {cm(kMJPG, 1080, 1920, {30}), cm(kMJPG, 960, 1280, {30}), cm(kMJPG, 720, 1280, {30}),
            cm(kMJPG, 600, 800, {30}),   cm(kMJPG, 576, 1024, {30}), cm(kMJPG, 480, 640, {30})};
}

void text_and_parse()
{
    CHECK_EQ(fourcc_text(kMJPG), std::string("MJPG"));
    CHECK_EQ(fourcc_text(kYUYV), std::string("YUYV"));
    CHECK_EQ(fourcc_text(0), std::string("????"));
    ModeReq m;
    CHECK(parse_mode("MJPG:1280x720:30", m));
    CHECK_EQ(m.fourcc, kMJPG);
    CHECK_EQ(m.w, 1280);
    CHECK_EQ(m.h, 720);
    CHECK_EQ(m.fps, 30);
    CHECK(parse_mode("yuyv:640x360", m));
    CHECK_EQ(m.fourcc, kYUYV);
    CHECK_EQ(m.h, 360);
    CHECK_EQ(m.fps, 30);
    CHECK(parse_mode("MJPG:1920x1080@60", m));
    CHECK_EQ(m.fps, 60);
    CHECK_EQ(mode_text(m), std::string("MJPG 1920x1080 60 fps"));
    CHECK_EQ(mode_arg(m), std::string("MJPG:1920x1080:60"));
    ModeReq back;
    CHECK(parse_mode(mode_arg(m), back));
    CHECK(back.same(m));
    ModeReq bad;
    CHECK(!parse_mode("", bad));
    CHECK(!parse_mode("H264:1280x720:30", bad));
    CHECK(!parse_mode("MJPG:1280x720:", bad));
    CHECK(!parse_mode("MJPG:1280x720:30x", bad));
    CHECK(!parse_mode("MJPG:1280:30", bad));
    CHECK(!parse_mode("MJPG:0x720:30", bad));
    CHECK(!parse_mode("MJPG:1280x720:0", bad));
    CHECK(!parse_mode("MJPG-1280x720-30", bad));
    CHECK(!bad.valid());
    CHECK_EQ(interval_fps(1, 30), 30);
    CHECK_EQ(interval_fps(333333, 10000000), 30);
    CHECK_EQ(interval_fps(1, 60), 60);
    CHECK_EQ(interval_fps(0, 30), 0);
    CHECK_EQ(interval_fps(1, 0), 0);
}

void resolve()
{
    const std::vector<CamMode> b = position_b();
    ModeReq want, got;
    CHECK(parse_mode("MJPG:1280x720:30", want));
    CHECK(resolve_mode(b, want, got));
    CHECK(got.same(want));
    CHECK(parse_mode("MJPG:1280x720:25", want));
    CHECK(resolve_mode(b, want, got));
    CHECK_EQ(got.fps, 30); /* the nearest listed */
    CHECK(parse_mode("YUYV:640x360:60", want));
    CHECK(resolve_mode(b, want, got));
    CHECK_EQ(got.fps, 30);
    CHECK(parse_mode("YUYV:1280x720:30", want));
    CHECK(!resolve_mode(b, want, got)); /* no YUYV 720p */
    CHECK(parse_mode("MJPG:640x360:30", want));
    CHECK(!resolve_mode(b, want, got)); /* no MJPEG 640x360 on this camera (P0-b) */
    CHECK(parse_mode("YUYV:640x480:30", want));
    CHECK(!resolve_mode(position_a(), want, got)); /* no YUYV in portrait */
}

void lists()
{
    CHECK(lists_format(position_b(), kMJPG));
    CHECK(lists_format(position_b(), kYUYV));
    CHECK(!lists_format(position_a(), kYUYV));
    CHECK(position_b()[0].has_fps(30));
    CHECK(!position_a()[0].has_fps(60));
    const std::string root = make_temp_dir("dev");
    CHECK(list_dir(root + "/nothing").empty());
    write_file(root + "/b", "");
    write_file(root + "/a", "");
    const std::vector<std::string> l = list_dir(root);
    CHECK_EQ(l.size(), static_cast<std::size_t>(2));
    if (l.size() == 2) CHECK_EQ(l[0], std::string("a")); /* sorted */
    rm_tree(root);
}

} // namespace

void test_modes()
{
    text_and_parse();
    resolve();
    lists();
}
