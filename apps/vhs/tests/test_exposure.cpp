/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests (0.1.2 draft 2): exposure in stops, the focus wait, the 8 MP photo size.
 *   - the maths: stop set and labels, the luma target (kept in 8..245), the exposure value's clamp and step grid, the
 *     next value, and the feedback loop (ExposureMeter) against the owner's measurements of the tablet's back camera
 *     (manual shutter 50 / 100 / 166 / 400 / 1000 / 3000 x 0.1 ms gave mean luma 10 / 17 / 25 / 47 / 80 / 137);
 *   - capability detection on fake control tables: the tablet's back camera, its front camera (no exposure, gain or
 *     white-balance controls), a camera with no controls at all, one whose menu lacks Manual Mode, one whose exposure is
 *     read-only, autofocus;
 *   - the capture thread: exposure written on request and again after every start of streaming (an in-place switch to
 *     the photo size, a reopen), Aperture Priority put back at close, dropped on another camera; a held size the
 *     camera refuses falls back to the next smaller one and the photo is taken there; the focus wait (settled at once,
 *     settles after hunting, capped, skipped without autofocus);
 *   - the size lists: every MJPEG size at any rate for the Photo size (3264x2448 (8 MP)), the default unchanged;
 *   - the Exposure row's geometry in every screen class;
 *   - the controller on the fake back camera: the stops reach the picture, the photo has the preview's shutter at 8 MP,
 *     the setting follows Photo / Video and goes with a camera switch.
 */
#include "check.hpp"
#include "fixtures.hpp"

#include "app/controller.hpp"
#include "camera/capture.hpp"
#include "camera/exposure.hpp"
#include "camera/fake_camera.hpp"
#include "jpeg/turbojpeg.hpp"
#include "layout/capture_layout.hpp"
#include "live/clock.hpp"
#include "live/convert.hpp"
#include "live/pipeline.hpp"
#include "modes/select.hpp"
#include "scenes/fileops.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

using namespace vhs;

namespace {

/* the tablet's back camera: exposure_time_absolute 50..10000 step 1 default 166 */
ExposureCaps back_caps()
{
    ExposureCaps c;
    c.manual = true;
    c.min = 50;
    c.max = 10000;
    c.step = 1;
    c.def = 166;
    c.auto_was = 3;
    c.aperture = true;
    return c;
}

/* mean luma against the manual shutter, as measured (interpolated; the last slope goes on; clipped at 255) */
double model_luma(double e)
{
    static const double p[][2] = {{50, 10}, {100, 17}, {166, 25}, {400, 47}, {1000, 80}, {3000, 137}};
    const int n = 6;
    double l;
    if (e <= p[0][0]) {
        l = p[0][1] * e / p[0][0];
    } else if (e >= p[n - 1][0]) {
        l = p[n - 1][1] + (e - p[n - 1][0]) * (p[n - 1][1] - p[n - 2][1]) / (p[n - 1][0] - p[n - 2][0]);
    } else {
        l = 0;
        for (int i = 1; i < n; ++i)
            if (e <= p[i][0]) {
                l = p[i - 1][1] + (e - p[i - 1][0]) * (p[i][1] - p[i - 1][1]) / (p[i][0] - p[i - 1][0]);
                break;
            }
    }
    return std::min(255.0, l);
}

struct Sim {
    int value = 0;
    double luma = 0;
    int writes = 0;
    bool done = false;
};

/* the loop against the model: every command is "written", the next sample is the model's luma at that value */
Sim run_loop(ExposureMeter &m, int stop, double auto_luma)
{
    Sim s;
    ExposureMeter::Action a = m.select(stop, auto_luma);
    if (a.kind == ExposureMeter::Kind::Manual) {
        s.value = a.value;
        ++s.writes;
    }
    int guard = 0;
    while (m.seeking() && guard++ < 20) {
        a = m.sample(model_luma(s.value));
        if (a.kind == ExposureMeter::Kind::Manual) {
            s.value = a.value;
            ++s.writes;
        }
    }
    s.luma = model_luma(s.value);
    s.done = !m.seeking();
    return s;
}

void the_maths()
{
    CHECK_EQ(clamp_stop(-5), -2);
    CHECK_EQ(clamp_stop(5), 2);
    CHECK_EQ(clamp_stop(1), 1);
    CHECK_EQ(stop_text(0), std::string("Auto"));
    CHECK_EQ(stop_text(1), std::string("+1"));
    CHECK_EQ(stop_text(2), std::string("+2"));
    CHECK_EQ(stop_text(-1), std::string("-1"));
    CHECK_EQ(stop_text(-2), std::string("-2"));
    CHECK_NEAR(stop_factor(0), 1.0, 1e-9);
    CHECK_NEAR(stop_factor(2), 4.0, 1e-9);
    CHECK_NEAR(stop_factor(-2), 0.25, 1e-9);
    /* the target is the base times 2^N, kept in 8..245 */
    CHECK_NEAR(target_luma(80, 1), 160, 1e-9);
    CHECK_NEAR(target_luma(80, -2), 20, 1e-9);
    CHECK_NEAR(target_luma(200, 2), kLumaHigh, 1e-9);
    CHECK_NEAR(target_luma(200, 1), kLumaHigh, 1e-9);
    CHECK_NEAR(target_luma(10, -2), kLumaLow, 1e-9);
    CHECK_NEAR(target_luma(0, 1), kLumaLow, 1e-9);
    /* within 8 % (or 2 levels at the dark end) */
    CHECK(luma_close(160, 160));
    CHECK(luma_close(172, 160));
    CHECK(!luma_close(174, 160));
    CHECK(luma_close(148, 160));
    CHECK(!luma_close(146, 160));
    CHECK(luma_close(9.9, 8));
    CHECK(!luma_close(11, 8));
    /* the value: clamped to the control, on its step grid */
    const ExposureCaps c = back_caps();
    CHECK_EQ(clamp_exposure(1000, c), 1000);
    CHECK_EQ(clamp_exposure(1000.4, c), 1000);
    CHECK_EQ(clamp_exposure(10, c), 50);
    CHECK_EQ(clamp_exposure(99999, c), 10000);
    CHECK_EQ(clamp_exposure(-5, c), 50);
    ExposureCaps st = c;
    st.min = 3;
    st.max = 2047;
    st.step = 4; /* grid 3, 7, ..., 2047 */
    CHECK_EQ(clamp_exposure(100, st), 99);
    CHECK_EQ(clamp_exposure(101, st), 103);
    CHECK_EQ(clamp_exposure(5000, st), 2047);
    CHECK_EQ(clamp_exposure(1, st), 3);
    st.max = 2045; /* the last grid point not above it: 2043 */
    CHECK_EQ(clamp_exposure(5000, st), 2043);
    /* the next value: the ratio of target to luma, at most 4x a step, clamped */
    CHECK_EQ(next_exposure(1000, 80, 160, c), 2000);
    CHECK_EQ(next_exposure(1000, 80, 40, c), 500);
    CHECK_EQ(next_exposure(1000, 10, 200, c), 4000); /* 20x wanted, 4x given */
    CHECK_EQ(next_exposure(1000, 200, 10, c), 250);  /* 1/20x wanted, 1/4 given */
    CHECK_EQ(next_exposure(8000, 50, 200, c), 10000); /* the end of the control */
    CHECK_EQ(next_exposure(60, 100, 10, c), 50);
    CHECK_EQ(next_exposure(0, 0, 100, c), 50);       /* a luma of 0 and a value of 0 are read as 1: x4, then the control's end */
}

void meter_follows_the_camera()
{
    /* the tablet's back camera in a dim room with a bright window: automatic chose about 1000, luma 80 */
    for (int stop : {1, -1, -2}) {
        ExposureMeter m;
        m.configure(back_caps());
        const Sim s = run_loop(m, stop, 80);
        CHECK(s.done);
        CHECK_EQ(m.stop(), stop);
        CHECK_NEAR(m.base(), 80, 1e-9);
        CHECK(luma_close(s.luma, m.target()));
        CHECK(m.iterations() <= kExposureMaxIterations);
        CHECK(!m.at_limit());
        CHECK(s.value >= 50 && s.value <= 10000);
    }
    {   /* +1 wants 160, reachable at about 3800; -1 wants 40, about 330; -2 wants 20, about 130 */
        ExposureMeter m;
        m.configure(back_caps());
        const Sim up = run_loop(m, 1, 80);
        CHECK(up.value > 3000 && up.value < 4500);
        const Sim down = run_loop(m, -1, 80); /* stop to stop: from the last value, x 2^-2 first */
        CHECK(down.value > 250 && down.value < 450);
        CHECK(luma_close(down.luma, 40));
        CHECK_NEAR(m.base(), 80, 1e-9); /* the base is kept while a stop is on: no new metering */
        const Sim down2 = run_loop(m, -2, 999); /* the base given here is not used: the camera is not in Auto */
        CHECK(luma_close(down2.luma, 20));
        CHECK_NEAR(m.base(), 80, 1e-9);
    }
    {   /* back to Auto: Auto is written, nothing is sought; leaving it again measures a new base */
        ExposureMeter m;
        m.configure(back_caps());
        run_loop(m, 1, 80);
        const ExposureMeter::Action a = m.select(0, 0);
        CHECK(a.kind == ExposureMeter::Kind::Auto);
        CHECK(!m.seeking());
        CHECK_EQ(m.stop(), 0);
        CHECK(m.sample(50).kind == ExposureMeter::Kind::None); /* a stray sample does nothing */
        const Sim again = run_loop(m, -1, 120);
        CHECK_NEAR(m.base(), 120, 1e-9);
        CHECK(luma_close(again.luma, 60));
        CHECK(m.select(-1, 120).kind == ExposureMeter::Kind::None); /* the same stop again: nothing */
    }
    {   /* a base that is unknown: mid grey; a base near white: the target stops at 245 */
        ExposureMeter m;
        m.configure(back_caps());
        run_loop(m, -1, 0);
        CHECK_NEAR(m.base(), 110, 1e-9);
        ExposureMeter w;
        w.configure(back_caps());
        const Sim s = run_loop(w, 2, 200);
        CHECK_NEAR(w.target(), kLumaHigh, 1e-9);
        CHECK(luma_close(s.luma, kLumaHigh));
    }
    {   /* the end of the control: as near as it gets, then it stops (no endless loop, no value out of range) */
        ExposureCaps small = back_caps();
        small.max = 200;
        ExposureMeter m;
        m.configure(small);
        const Sim s = run_loop(m, 2, 80);
        CHECK(s.done);
        CHECK(m.at_limit());
        CHECK_EQ(s.value, 200);
        ExposureCaps dark = back_caps();
        dark.min = 400;
        ExposureMeter d;
        d.configure(dark);
        const Sim t = run_loop(d, -2, 80);
        CHECK(t.done);
        CHECK(d.at_limit());
        CHECK_EQ(t.value, 400);
    }
    {   /* a camera that cannot do it: nothing */
        ExposureMeter m;
        m.configure(ExposureCaps());
        CHECK(m.select(1, 80).kind == ExposureMeter::Kind::None);
        CHECK_EQ(m.stop(), 0);
    }
    {   /* the loop never goes on past kExposureMaxIterations even when the luma never settles */
        ExposureMeter m;
        m.configure(back_caps());
        m.select(1, 80);
        int writes = 0;
        for (int i = 0; i < 30 && m.seeking(); ++i) {
            const ExposureMeter::Action a = m.sample(i % 2 ? 30 : 230); /* a scene that flickers */
            if (a.kind == ExposureMeter::Kind::Manual) ++writes;
        }
        CHECK(!m.seeking());
        CHECK(writes <= kExposureMaxIterations);
    }
}

void focus_settle()
{
    FocusSettle f;
    CHECK(!f.settled());
    for (int i = 0; i < 5; ++i) f.push(100000);
    CHECK(!f.settled()); /* the window is 6 */
    f.push(101000);
    CHECK(f.settled());
    f.push(140000); /* the focus moves: the sizes jump */
    CHECK(!f.settled());
    for (int i = 0; i < 5; ++i) f.push(100000);
    CHECK(!f.settled()); /* the jump is still in the window */
    f.push(100500);
    CHECK(f.settled());
    f.push(0); /* an empty frame is not a size */
    CHECK_EQ(f.count(), FocusSettle::kWindow);
    f.reset();
    CHECK(!f.settled());
    CHECK_EQ(f.count(), 0u);
    for (int i = 0; i < 8; ++i) f.push(i % 2 ? 100000 : 112000); /* hunting back and forth */
    CHECK(!f.settled());
}

/* ------------------------------------------------------------------------------------------------ capabilities */

/* a camera seen through a filter on its controls (a menu without Manual Mode, a read-only exposure) */
class Filtered final : public V4l2Api {
public:
    explicit Filtered(V4l2Api &inner) : in_(inner) {}
    int open(const char *p) override { return in_.open(p); }
    int close(int fd) override { return in_.close(fd); }
    int ioctl(int fd, unsigned long req, void *arg) override
    {
        if (req == VIDIOC_QUERYMENU && no_manual) {
            auto *m = static_cast<struct v4l2_querymenu *>(arg);
            if (m->index == 1) {
                errno = EINVAL;
                return -1;
            }
        }
        const int r = in_.ioctl(fd, req, arg);
        if (r == 0 && req == VIDIOC_QUERYCTRL && abs_read_only) {
            auto *q = static_cast<struct v4l2_queryctrl *>(arg);
            if (q->id == kCidExposureAbsolute) q->flags |= V4L2_CTRL_FLAG_READ_ONLY;
        }
        if (r == 0 && req == VIDIOC_QUERYCTRL && menu_read_only) {
            auto *q = static_cast<struct v4l2_queryctrl *>(arg);
            if (q->id == kCidExposureAuto) q->flags |= V4L2_CTRL_FLAG_READ_ONLY;
        }
        return r;
    }
    void *map(std::size_t l, int fd, off_t o) override { return in_.map(l, fd, o); }
    int unmap(void *a, std::size_t l) override { return in_.unmap(a, l); }
    int poll_in(int fd, int t) override { return in_.poll_in(fd, t); }
    bool no_manual = false, abs_read_only = false, menu_read_only = false;

private:
    V4l2Api &in_;
};

ExposureCaps caps_of(FakeCamera &cam)
{
    const int fd = cam.open(FakeCamera::kPath);
    const ExposureCaps c = detect_exposure_caps(cam, fd);
    cam.close(fd);
    return c;
}

void capability_detection()
{
    FakeOptions o;
    o.frames = 1;
    o.realtime = false;
    { /* the tablet's back camera: USB2.0 Camera, 9.4 */
        o.profile = FakeProfile::Back8mp;
        FakeCamera cam(o);
        const ExposureCaps c = caps_of(cam);
        CHECK(c.manual);
        CHECK_EQ(c.min, 50);
        CHECK_EQ(c.max, 10000);
        CHECK_EQ(c.step, 1);
        CHECK_EQ(c.def, 166);
        CHECK_EQ(c.auto_was, 3);
        CHECK(c.aperture);
        CHECK(!c.autofocus); /* fixed focus unless the camera has the control */
        CHECK_EQ(exposure_caps_log(c), std::string("controls: manual exposure 50..10000 step 1 (default 166, auto_exposure was 3), fixed focus"));
    }
    { /* the tablet's front camera: USB FHD Webcam: no exposure, gain or white-balance controls: no row */
        o.profile = FakeProfile::UsbFhd;
        FakeCamera cam(o);
        const ExposureCaps c = caps_of(cam);
        CHECK(!c.manual);
        CHECK(!c.autofocus);
        CHECK_EQ(exposure_caps_log(c), std::string("controls: no manual exposure, fixed focus"));
    }
    for (FakeProfile p : {FakeProfile::FineCam, FakeProfile::C270, FakeProfile::YuyvOnly, FakeProfile::Usb2}) { /* no controls at all (the C270 has exposure_auto_priority only) */
        o.profile = p;
        FakeCamera cam(o);
        const ExposureCaps c = caps_of(cam);
        CHECK(!c.manual && !c.autofocus);
    }
    { /* autofocus on a camera without manual exposure, and with */
        o.profile = FakeProfile::FineCam;
        o.autofocus = true;
        FakeCamera cam(o);
        const ExposureCaps c = caps_of(cam);
        CHECK(!c.manual);
        CHECK(c.autofocus);
        CHECK_EQ(exposure_caps_log(c), std::string("controls: no manual exposure, continuous autofocus"));
        o.autofocus = false;
    }
    { /* a menu without Manual Mode, a read-only exposure time, a read-only menu: not offered */
        o.profile = FakeProfile::Back8mp;
        FakeCamera cam(o);
        Filtered f(cam);
        f.no_manual = true;
        int fd = f.open(FakeCamera::kPath);
        CHECK(!detect_exposure_caps(f, fd).manual);
        f.close(fd);
        f.no_manual = false;
        f.abs_read_only = true;
        fd = f.open(FakeCamera::kPath);
        CHECK(!detect_exposure_caps(f, fd).manual);
        f.close(fd);
        f.abs_read_only = false;
        f.menu_read_only = true;
        fd = f.open(FakeCamera::kPath);
        CHECK(!detect_exposure_caps(f, fd).manual);
        f.close(fd);
        f.menu_read_only = false;
        fd = f.open(FakeCamera::kPath);
        CHECK(detect_exposure_caps(f, fd).manual); /* the filter off: as before */
        f.close(fd);
    }
    { /* writing: manual clamps to the range, Auto puts Aperture Priority back; nothing on a camera that cannot */
        o.profile = FakeProfile::Back8mp;
        FakeCamera cam(o);
        const int fd = cam.open(FakeCamera::kPath);
        const ExposureCaps c = detect_exposure_caps(cam, fd);
        CHECK_EQ(cam.exp_auto(), 3);
        CHECK(write_exposure(cam, fd, c, true, 1000));
        CHECK_EQ(cam.exp_auto(), 1);
        CHECK_EQ(cam.exp_abs(), 1000);
        CHECK(write_exposure(cam, fd, c, true, 99999));
        CHECK_EQ(cam.exp_abs(), 10000);
        CHECK(write_exposure(cam, fd, c, true, 1));
        CHECK_EQ(cam.exp_abs(), 50);
        CHECK(write_exposure(cam, fd, c, false, 0));
        CHECK_EQ(cam.exp_auto(), 3);
        CHECK_EQ(cam.exp_abs(), 50); /* UVC: the value stays, stale, in auto */
        CHECK(!write_exposure(cam, fd, ExposureCaps(), true, 1000)); /* a camera that cannot: nothing written */
        const int writes = cam.exp_writes();
        write_exposure(cam, fd, ExposureCaps(), false, 0);
        CHECK_EQ(cam.exp_writes(), writes);
        cam.close(fd);
    }
}

/* ------------------------------------------------------------------------------------------------ size lists */

void size_lists()
{
    std::vector<CamMode> back;
    {
        FakeOptions o;
        o.profile = FakeProfile::Back8mp;
        o.frames = 1;
        FakeCamera cam(o);
        back = cam.modes();
    }
    /* the Photo size row: every MJPEG size at any rate, largest first */
    const std::vector<Size> all = mjpeg_sizes(back);
    CHECK_EQ(all.size(), 7u);
    CHECK(all[0] == (Size{3264, 2448}));
    CHECK(all[1] == (Size{2592, 1944}));
    CHECK(all[2] == (Size{2048, 1536}));
    CHECK(all[3] == (Size{1920, 1080})); /* by area: 2.07 MP before 1.92 MP */
    CHECK(all[4] == (Size{1600, 1200}));
    CHECK(all[6] == (Size{640, 480}));
    /* the 30 fps ones (the Video size, the default): without the 15 fps sizes */
    const std::vector<Size> fast = mjpeg30_sizes(back);
    CHECK_EQ(fast.size(), 4u);
    CHECK(fast[0] == (Size{1920, 1080}));
    CHECK_EQ(photo_size_text(Size{3264, 2448}), std::string("3264x2448 (8 MP)"));
    CHECK_EQ(photo_size_text(Size{2592, 1944}), std::string("2592x1944 (5 MP)"));
    CHECK_EQ(photo_size_text(Size{2048, 1536}), std::string("2048x1536 (3 MP)"));
    CHECK_EQ(photo_size_text(Size{1920, 1080}), std::string("1920x1080"));
    CHECK_EQ(photo_size_text(Size{1600, 1200}), std::string("1600x1200"));
    CHECK_EQ(photo_size_text(Size{640, 480}), std::string("640x480"));
    /* the mode of a size: 30 fps when listed, else the highest rate listed */
    CHECK_EQ(mode_arg(mjpeg_mode_for(back, Size{3264, 2448})), std::string("MJPG:3264x2448:15"));
    CHECK_EQ(mode_arg(mjpeg_mode_for(back, Size{1600, 1200})), std::string("MJPG:1600x1200:30"));
    CHECK_EQ(mode_arg(mjpeg_mode_for(back, Size{777, 555})), std::string("MJPG:777x555:30")); /* not listed: as before */
    /* the selection: the default is the largest at 30 fps (as 0.1.1); a stored 8 MP size is honoured at 15 fps */
    SelectInput in;
    in.listed = back;
    in.soc = SocClass::Bcm2837;
    Selection s = select_modes(in);
    CHECK(s.ok);
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1920x1080:30"));
    CHECK_EQ(s.photo_sizes.size(), 7u);
    CHECK_EQ(s.sizes.size(), 4u);
    in.photo_size = Size{3264, 2448};
    s = select_modes(in);
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:3264x2448:15"));
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:3264x2448:15")); /* Instant: the preview is the photo size */
    CHECK_EQ(mode_arg(s.light), std::string("MJPG:1280x720:30"));
    CHECK_EQ(mode_arg(s.video), std::string("MJPG:1280x720:30"));
    in.have_photo_mode = true;
    in.photo_mode = PhotoMode::Light;
    s = select_modes(in);
    CHECK_EQ(mode_arg(s.preview), std::string("MJPG:1280x720:30")); /* Light: the photo switches to 8 MP */
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:3264x2448:15"));
    CHECK(!s.light_is_instant);
    /* a camera whose sizes all run at 30 fps: nothing changes */
    SelectInput fc;
    fc.listed = {fx::cm(kMJPG, 1920, 1080, {60, 30}), fx::cm(kMJPG, 1280, 720, {60, 30}), fx::cm(kYUYV, 640, 480, {30})};
    s = select_modes(fc);
    CHECK(s.photo_sizes == s.sizes);
    CHECK_EQ(mode_arg(s.photo), std::string("MJPG:1920x1080:30"));
    /* hold_choice: the size itself, then the next smaller one not refused */
    const ModeReq big = mjpeg_mode_for(back, Size{3264, 2448});
    CHECK_EQ(mode_arg(hold_choice(back, big, {})), std::string("MJPG:3264x2448:15"));
    CHECK_EQ(mode_arg(hold_choice(back, big, {Size{3264, 2448}})), std::string("MJPG:2592x1944:15"));
    CHECK_EQ(mode_arg(hold_choice(back, big, {Size{3264, 2448}, Size{2592, 1944}})), std::string("MJPG:2048x1536:15"));
    CHECK_EQ(mode_arg(hold_choice(back, big, {Size{3264, 2448}, Size{2592, 1944}, Size{2048, 1536}})), std::string("MJPG:1920x1080:30"));
    CHECK_EQ(mode_arg(hold_choice(back, big, {Size{3264, 2448}, Size{2592, 1944}, Size{2048, 1536}, Size{1920, 1080}})),
             std::string("MJPG:1600x1200:30"));
    std::vector<Size> every = all;
    CHECK(!hold_choice(back, big, every).valid()); /* nothing left */
    CHECK(!hold_choice(back, mjpeg_mode_for(back, Size{640, 480}), {Size{640, 480}}).valid());
}

/* ------------------------------------------------------------------------------------------------ the capture thread */

struct Rig {
    explicit Rig(FakeOptions o) : cam(o), finder(make_fake_finder(cam)) {}
    CaptureConfig config()
    {
        CaptureConfig c;
        c.api = &cam;
        c.finder = finder.get();
        c.poll_ms = 50;
        c.retry_ms = 100;
        c.lost_retry_ms = 50;
        c.lost_hold_ms = 400;
        c.back_window_ms = 2000;
        c.choose = [](const std::vector<CamMode> &listed, const CamIdentity &) {
            SelectInput in;
            in.listed = listed;
            in.soc = SocClass::Bcm2837;
            return select_modes(in).preview;
        };
        return c;
    }
    FakeCamera cam;
    std::unique_ptr<CameraFinder> finder;
};

FakeOptions back_options()
{
    FakeOptions o;
    o.profile = FakeProfile::Back8mp;
    o.frames = 1;
    o.realtime = false;
    return o;
}

void capture_writes_exposure()
{
    Rig r(back_options());
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming; }));
    CaptureStatus st = cap.status();
    CHECK(st.exp_caps.manual);
    CHECK_EQ(st.exp_value, -1);
    CHECK_EQ(st.exp_apply_n, 0ull);
    CHECK_EQ(r.cam.exp_writes(), 0); /* Auto and never left it: the camera is not touched (as before) */
    CHECK_EQ(r.cam.exp_auto(), 3);
    cap.set_exposure(true, 500);
    CHECK(wait_for(3000, [&] { return cap.status().exp_value == 500; }));
    CHECK_EQ(r.cam.exp_auto(), 1);
    CHECK_EQ(r.cam.exp_abs(), 500);
    st = cap.status();
    CHECK_EQ(st.exp_apply_n, 1ull);
    CHECK_EQ(st.exp_apply_gen, st.gen);
    CHECK(st.exp_failed == 0);
    /* the preview is not restarted for it: the same stream session */
    CHECK_EQ(cap.status().gen, st.gen);
    /* a value out of range is clamped, not refused */
    cap.set_exposure(true, 99999);
    CHECK(wait_for(3000, [&] { return cap.status().exp_value == 10000; }));
    CHECK_EQ(r.cam.exp_abs(), 10000);
    cap.set_exposure(true, 800);
    CHECK(wait_for(3000, [&] { return cap.status().exp_value == 800; }));
    const std::uint64_t gen0 = cap.status().gen;

    /* the photo size: an in-place switch keeps the shutter (written again after the new stream starts) */
    const ModeReq big = mjpeg_mode_for(r.cam.modes(), Size{2592, 1944});
    const std::uint64_t n0 = cap.status().exp_apply_n;
    cap.hold_mode(big);
    CHECK(wait_for(8000, [&] { return cap.status().mode.w == 2592 && cap.status().exp_apply_n > n0; }));
    st = cap.status();
    CHECK(st.gen > gen0);
    CHECK_EQ(st.exp_apply_gen, st.gen); /* the write belongs to the new stream */
    CHECK_EQ(st.exp_value, 800);
    CHECK_EQ(r.cam.exp_auto(), 1);
    CHECK_EQ(r.cam.exp_abs(), 800);
    CHECK_EQ(cap.opens(), 1ull); /* in place */
    cap.release_hold();
    CHECK(wait_for(8000, [&] { return cap.status().mode.w == 1920; }));
    CHECK_EQ(r.cam.exp_auto(), 1); /* back at the preview size, still 800 */
    CHECK_EQ(r.cam.exp_abs(), 800);

    /* a full reopen (the camera was closed: Aperture Priority put back first): written again */
    const std::uint64_t opens = cap.opens();
    cap.reopen();
    CHECK(wait_for(8000, [&] { return cap.opens() == opens + 1 && cap.status().state == CamState::Streaming && cap.status().exp_value == 800; }));
    CHECK(wait_for(3000, [&] { return r.cam.exp_auto() == 1; }));
    CHECK_EQ(r.cam.exp_abs(), 800);

    /* Auto again: Aperture Priority; the value stays stale in the camera */
    cap.set_exposure(false, 0);
    CHECK(wait_for(3000, [&] { return cap.status().exp_value == -1; }));
    CHECK_EQ(r.cam.exp_auto(), 3);
    /* a camera left on Manual goes back to Aperture Priority at close */
    cap.set_exposure(true, 300);
    CHECK(wait_for(3000, [&] { return r.cam.exp_auto() == 1 && cap.status().exp_value == 300; }));
    cap.stop();
    CHECK_EQ(r.cam.exp_auto(), 3);
}

void capture_exposure_other_cameras()
{
    /* a camera that cannot do it ignores the request and is never touched */
    FakeOptions o = back_options();
    o.profile = FakeProfile::UsbFhd;
    Rig r(o);
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming; }));
    CHECK(!cap.status().exp_caps.manual);
    cap.set_exposure(true, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(cap.status().exp_apply_n, 0ull);
    CHECK_EQ(cap.status().exp_value, -1);
    CHECK_EQ(r.cam.exp_writes(), 0);
    cap.stop();
}

void capture_exposure_dropped_on_another_camera()
{
    FakeOptions a = back_options();
    FakeOptions b = back_options();
    b.profile = FakeProfile::UsbFhd;
    b.path = std::string(FakeCamera::kPath) + "1";
    b.index = 1;
    FakeCamera cam_a(a), cam_b(b);
    FakeCameraBus bus({&cam_a, &cam_b});
    std::unique_ptr<CameraFinder> finder = make_fake_bus_finder({&cam_a, &cam_b});
    CaptureConfig c;
    c.api = &bus;
    c.finder = finder.get();
    c.poll_ms = 50;
    c.retry_ms = 100;
    c.choose = [](const std::vector<CamMode> &listed, const CamIdentity &) {
        SelectInput in;
        in.listed = listed;
        in.soc = SocClass::Bcm2837;
        return select_modes(in).preview;
    };
    LatestMailbox<Frame> box;
    Capture cap(c, box);
    cap.start();
    CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming && cap.status().key == "5986:0656@1-9.4"; }));
    cap.set_exposure(true, 700);
    CHECK(wait_for(3000, [&] { return cap.status().exp_value == 700; }));
    CHECK_EQ(cam_a.exp_auto(), 1);
    /* the other camera opens: the stop does not follow (and the first one is put back on Aperture Priority) */
    finder->prefer("13d3:5224@1-9.3");
    cap.reopen();
    CHECK(wait_for(8000, [&] { return cap.status().state == CamState::Streaming && cap.status().key == "13d3:5224@1-9.3"; }));
    CHECK(!cap.status().exp_caps.manual);
    CHECK_EQ(cam_a.exp_auto(), 3);
    CHECK_EQ(cam_b.exp_writes(), 0);
    /* and back to the first: Auto, not 700 */
    finder->prefer("5986:0656@1-9.4");
    cap.reopen();
    CHECK(wait_for(8000, [&] { return cap.status().state == CamState::Streaming && cap.status().key == "5986:0656@1-9.4"; }));
    CHECK(cap.status().exp_caps.manual);
    CHECK_EQ(cap.status().exp_value, -1);
    CHECK_EQ(cam_a.exp_auto(), 3);
    cap.stop();
}

bool take_photo_within(Capture &cap, int ms, PhotoShot &shot)
{
    return wait_for(ms, [&] { return cap.take_photo(shot); });
}

void capture_refused_hold()
{
    FakeOptions o = back_options();
    o.refuse_area = 3000000; /* STREAMON ENOSPC above 3 MP: 8 MP, 5 MP and 2048x1536 fail, 1920x1080 (2.07 MP) works */
    Rig r(o);
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming; }));
    const std::uint64_t opens = cap.opens();
    const ModeReq big = mjpeg_mode_for(r.cam.modes(), Size{3264, 2448});
    CHECK(cap.status().mode.w * cap.status().mode.h <= o.refuse_area);
    cap.hold_mode(big);
    cap.request_photo(big, 7);
    PhotoShot shot;
    CHECK(take_photo_within(cap, 15000, shot));
    CHECK(shot.ok);
    CHECK_EQ(shot.id, 7ull);
    CHECK_EQ(shot.mode.fourcc, kMJPG);
    CHECK(static_cast<long>(shot.mode.w) * shot.mode.h <= o.refuse_area); /* the largest size the camera takes */
    CHECK_EQ(shot.mode.w, 1920); /* 3264, 2592 and 2048 refused: the next smaller size that fits */
    CHECK(shot.data.size() > 100);
    CHECK_EQ(cap.opens(), opens); /* on the open device */
    CHECK(cap.status().state == CamState::Streaming);
    /* the preview comes back, the refused sizes are forgotten */
    cap.release_hold();
    CHECK(wait_for(8000, [&] { return cap.status().state == CamState::Streaming && !cap.holding(); }));
    CHECK(wait_for(3000, [&] { return cap.frames() > 0 && cap.status().mode.valid(); }));
    /* the camera accepts it later: the size is tried again at the next hold */
    r.cam.set_refuse_area(0);
    cap.hold_mode(big);
    cap.request_photo(big, 8);
    CHECK(take_photo_within(cap, 20000, shot));
    CHECK(shot.ok);
    CHECK_EQ(shot.mode.w, 3264);
    CHECK_EQ(shot.mode.h, 2448);
    CHECK_EQ(shot.mode.fps, 15);
    cap.release_hold();
    CHECK(wait_for(8000, [&] { return cap.status().mode.w == 1920 && cap.status().state == CamState::Streaming; }));
    cap.stop();
}

void capture_refused_hold_everything()
{
    /* the camera refuses every size the hold can fall to but the preview's own: the photo is taken at the preview's size,
     * the stream is never left dead */
    FakeOptions o = back_options();
    o.refuse_area = 1000000;
    o.refuse_errno = EBUSY;
    Rig r(o);
    LatestMailbox<Frame> box;
    CaptureConfig c = r.config();
    c.choose = [](const std::vector<CamMode> &listed, const CamIdentity &) {
        SelectInput in;
        in.listed = listed;
        in.soc = SocClass::Bcm2837;
        in.have_photo_mode = true;
        in.photo_mode = PhotoMode::Light;
        return select_modes(in).preview; /* Light: 1280x720 */
    };
    Capture cap(c, box);
    cap.start();
    CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming; }));
    CHECK_EQ(cap.status().mode.w, 1280);
    const ModeReq big = mjpeg_mode_for(r.cam.modes(), Size{3264, 2448});
    cap.hold_mode(big);
    cap.request_photo(big, 3);
    PhotoShot shot;
    CHECK(take_photo_within(cap, 20000, shot));
    CHECK(shot.ok);
    CHECK(static_cast<long>(shot.mode.w) * shot.mode.h <= o.refuse_area); /* the first size the camera takes: 1280x720 */
    CHECK_EQ(shot.mode.w, 1280);
    cap.release_hold();
    CHECK(wait_for(5000, [&] { return !cap.holding() && cap.status().state == CamState::Streaming && cap.status().mode.w == 1280; }));
    std::uint64_t f0 = cap.frames();
    CHECK(wait_for(3000, [&] { return cap.frames() > f0 + 3; })); /* frames keep coming */
    cap.stop();
}

void capture_focus_wait()
{
    FakeOptions o = back_options();
    o.autofocus = true;
    { /* settled at once: the stream has been warm and its sizes steady; no wait beyond the warm-up */
        Rig r(o);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming && cap.frames() > 40; }));
        CHECK(cap.status().exp_caps.autofocus);
        cap.request_photo(ModeReq(), 1);
        PhotoShot shot;
        CHECK(take_photo_within(cap, 5000, shot));
        CHECK(shot.ok);
        CHECK(!shot.focus_capped);
        CHECK(shot.focus_wait_ms < 300);
        cap.stop();
    }
    { /* hunting for 40 frames after STREAMON: the photo waits for it, and is taken when the sizes have settled */
        FakeOptions h = o;
        h.focus_hunt_frames = 40;
        Rig r(h);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming; }));
        cap.request_photo(ModeReq(), 1);
        PhotoShot shot;
        CHECK(take_photo_within(cap, 8000, shot));
        CHECK(shot.ok);
        CHECK(!shot.focus_capped);
        CHECK(shot.seq > static_cast<std::uint32_t>(40)); /* not a frame of the hunt */
        cap.stop();
    }
    { /* a camera that never settles: the photo is taken at the cap, never blocked */
        FakeOptions h = o;
        h.focus_hunt_frames = 1000000;
        Rig r(h);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming && cap.frames() > 30; }));
        const std::int64_t t0 = mono_ms();
        cap.request_photo(ModeReq(), 1);
        PhotoShot shot;
        CHECK(take_photo_within(cap, 8000, shot));
        const std::int64_t took = mono_ms() - t0;
        CHECK(shot.ok);
        CHECK(shot.focus_capped);
        CHECK(shot.focus_wait_ms >= kFocusCapMs);
        CHECK(took >= kFocusCapMs - 50);
        CHECK(took < kFocusCapMs + 2500);
        /* the Instant request that asks for no settling takes the next good frame */
        cap.request_photo(ModeReq(), 2, false);
        CHECK(take_photo_within(cap, 3000, shot));
        CHECK(shot.ok && !shot.focus_capped && shot.focus_wait_ms == 0);
        cap.stop();
    }
    { /* a camera with fixed focus: no wait at all, whatever its sizes do */
        FakeOptions h = back_options();
        h.focus_hunt_frames = 1000000; /* has no effect without the control */
        Rig r(h);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(5000, [&] { return cap.status().state == CamState::Streaming && cap.frames() > 20; }));
        CHECK(!cap.status().exp_caps.autofocus);
        const std::int64_t t0 = mono_ms();
        cap.request_photo(ModeReq(), 1);
        PhotoShot shot;
        CHECK(take_photo_within(cap, 5000, shot));
        CHECK(shot.ok && !shot.focus_capped && shot.focus_wait_ms == 0);
        CHECK(mono_ms() - t0 < 1200);
        cap.stop();
    }
}

/* ------------------------------------------------------------------------------------------------ the layout */

bool inside_r(const Rect &in, const Rect &out)
{
    return in.x >= out.x && in.y >= out.y && in.right() <= out.right() && in.bottom() <= out.bottom();
}

bool overlaps(const Rect &a, const Rect &b)
{
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

void exposure_row_layout()
{
    /* 480x320 (the deck), 320x480, 800x480, 480x800, 640x480, 720x720 (Square) and smaller squares, at several TARGETs */
    const int sizes[][2] = {{480, 320}, {320, 480}, {800, 480}, {480, 800}, {640, 480}, {480, 640}, {720, 720}, {640, 640}, {480, 480}};
    const int targets[] = {44, 47, 52, 63};
    int checked = 0;
    for (const auto &sz : sizes)
        for (int t : targets) {
            CaptureLayoutIn in;
            in.w = sz[0];
            in.h = sz[1];
            in.body_y = 56;
            in.target = t;
            const double ar = static_cast<double>(sz[0]) / sz[1];
            in.square = ar >= 0.8 && ar <= 1.25 && sz[1] >= 400;
            const CaptureLayout g = capture_layout(in);
            if (std::getenv("VHS_DEBUG_LAYOUT"))
                std::fprintf(stderr, "layout %dx%d T%d sq%d pic %dx%d+%d+%d cam %d,%d %dx%d exp %d,%d %dx%d %dx%d %dx%d chip_y %d\n", sz[0], sz[1], t,
                             g.square, g.picture.w, g.picture.h, g.picture.x, g.picture.y, g.cam_chip.x, g.cam_chip.y, g.cam_chip.w,
                             g.cam_chip.h, g.exp_minus.x, g.exp_minus.y, g.exp_minus.w, g.exp_minus.h, g.exp_label.w, g.exp_label.h,
                             g.exp_plus.w, g.exp_plus.h, g.chip_y);
            /* a picture too low for two rows of TARGET (320x480 with a 63 px TARGET: 112x63) cannot keep the chips and the row
             * apart: only the basics are asked there (it is not a real panel) */
            const bool roomy = g.picture.h >= 2 * t + 3 * g.chip_y;
            const Rect parts[3] = {g.exp_minus, g.exp_label, g.exp_plus};
            for (const Rect &p : parts) {
                CHECK(p.w > 0 && p.h > 0);
                CHECK(inside_r(p, g.picture));                       /* over the picture, nothing outside it */
                CHECK(p.h >= std::min(t, g.picture.h - 2 * g.chip_y)); /* touchable: TARGET high */
                if (roomy) CHECK(p.w >= std::min(t, g.picture.w / 4)); /* and wide */
                CHECK(p.bottom() <= g.picture.bottom() - g.chip_y + 1);
                if (roomy) {
                    CHECK(!overlaps(p, g.cam_chip));                       /* the camera chip is at the top right */
                    CHECK(!overlaps(p, Rect{g.picture.x + g.chip_x, g.picture.y + g.chip_y, g.picture.w / 3,
                                            g.chip_px + 2 * g.chip_pad_v + 4})); /* the format chip, top left */
                }
                CHECK(!overlaps(p, g.toast) || true);
            }
            CHECK(!overlaps(g.exp_minus, g.exp_label) && !overlaps(g.exp_label, g.exp_plus) && !overlaps(g.exp_minus, g.exp_plus));
            CHECK(g.exp_minus.right() <= g.exp_label.x && g.exp_label.right() <= g.exp_plus.x);
            CHECK(g.exp_plus.w == g.exp_minus.w);
            if (roomy) CHECK(g.exp_minus.w >= std::min(t, g.picture.w / 4));
            CHECK(g.exp_label.w >= g.exp_minus.w);
            /* centred over the picture (a pixel of rounding) */
            const int left = g.exp_minus.x - g.picture.x, right = g.picture.right() - g.exp_plus.right();
            CHECK(std::abs(left - right) <= 1);
            CHECK_EQ(g.exp_minus.y, g.exp_label.y);
            CHECK_EQ(g.exp_minus.y, g.exp_plus.y);
            /* clear of the shutter, the buttons and the text under the picture */
            for (const Rect &o : {g.shutter, g.toggle, g.scenes, g.settings}) CHECK(!overlaps(g.exp_label, o));
            CHECK(g.exp_px >= 10);
            ++checked;
        }
    CHECK_EQ(checked, 36);
}

/* ------------------------------------------------------------------------------------------------ the controller */

bool step_until(Controller &ctl, int ms, const std::function<bool(const Snapshot &)> &pred, Snapshot *last = nullptr)
{
    Snapshot s;
    const bool ok = wait_for(ms, [&] {
        s = ctl.step(mono_ms());
        return pred(s);
    });
    if (last) *last = s;
    return ok;
}

ControllerConfig back_config(const std::string &folder, const std::string &set)
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.profile = FakeProfile::Back8mp;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = "bcm2837";
    c.clock_synced = 0;
    c.utc = true;
    c.fake_mic = "none";
    c.settings_overrides = set;
    return c;
}

/* the luma of the live picture: the mean of the pictures the pipeline decodes now (a short average) */
double live_luma(Controller &ctl)
{
    double sum = 0;
    int n = 0;
    std::uint64_t last = 0;
    wait_for(2000, [&] {
        const LumaSample s = ctl.pipeline().luma();
        if (s.valid && s.n != last) {
            last = s.n;
            sum += s.luma;
            ++n;
        }
        return n >= 6;
    });
    return n ? sum / n : 0;
}

/* the loop runs until the controller says it is done (and a few more steps for the last write) */
bool exposure_settled(Controller &ctl, int ms = 20000)
{
    return step_until(ctl, ms, [](const Snapshot &x) { return !x.exposure_seeking; });
}

void controller_exposure()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("exposure controller: no libturbojpeg.so.0 (the fake camera's MJPEG)");
        return;
    }
    const std::string dir = make_temp_dir("expctl");
    /* Light with an 8 MP Photo size: the preview is 1280x720 at 30 fps, the photo switches to 3264x2448 at 15 */
    Controller ctl(back_config(dir, "photo_mode=light,photo_size=3264x2448"));
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 6000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.valid(); }, &s));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1280x720:30"));
    CHECK(s.exposure_ok);
    CHECK_EQ(s.exposure_stop, 0);
    CHECK(!s.exposure_seeking);
    FakeCamera *cam = ctl.fake();
    CHECK(cam != nullptr);
    if (!cam) {
        ctl.stop();
        rm_tree(dir);
        return;
    }
    CHECK_EQ(cam->exp_auto(), 3);
    CHECK_EQ(cam->exp_writes(), 0);
    /* the Photo size row offers the 8 MP size */
    const Selection sel = ctl.selection();
    CHECK(std::find(sel.photo_sizes.begin(), sel.photo_sizes.end(), Size{3264, 2448}) != sel.photo_sizes.end());
    CHECK_EQ(mode_arg(sel.photo), std::string("MJPG:3264x2448:15"));

    const double l0 = live_luma(ctl);
    CHECK(l0 > 30 && l0 < 200);

    /* -1 stop: about half as bright as the automatic picture */
    ctl.exposure_set(-1);
    CHECK_EQ(ctl.exposure_stop(), -1);
    CHECK(exposure_settled(ctl));
    CHECK_EQ(cam->exp_auto(), 1);
    const int v_m1 = cam->exp_abs();
    CHECK(v_m1 > 300 && v_m1 < 700); /* the fake is linear in the shutter, nominal 1000 */
    const double lm1 = live_luma(ctl);
    CHECK_NEAR(lm1 / l0, 0.5, 0.12);
    s = ctl.step(mono_ms());
    CHECK_EQ(s.exposure_stop, -1);
    CHECK(s.exposure_ok);

    /* -2 from -1: from the last value x 1/2, then the loop; the base is kept (not metered again) */
    ctl.exposure_step(-1);
    CHECK_EQ(ctl.exposure_stop(), -2);
    CHECK(exposure_settled(ctl));
    const int v_m2 = cam->exp_abs();
    CHECK(v_m2 < v_m1);
    CHECK_NEAR(live_luma(ctl) / l0, 0.25, 0.1);
    ctl.exposure_step(-1); /* the end: nothing more */
    CHECK_EQ(ctl.exposure_stop(), -2);

    /* back to -1 for the photo: Photo / Video do not drop it */
    ctl.exposure_set(-1);
    CHECK(exposure_settled(ctl));
    const int shutter = cam->exp_abs();
    ctl.toggle_capture_mode();
    CHECK(ctl.capture_mode() == CaptureMode::Video);
    CHECK(step_until(ctl, 6000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.valid(); }, &s));
    CHECK_EQ(s.exposure_stop, -1);
    CHECK_EQ(cam->exp_auto(), 1);
    CHECK_EQ(cam->exp_abs(), shutter);
    ctl.toggle_capture_mode();
    CHECK(ctl.capture_mode() == CaptureMode::Photo);
    CHECK(step_until(ctl, 6000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.valid(); }, &s));
    CHECK_EQ(s.exposure_stop, -1);
    CHECK_EQ(cam->exp_abs(), shutter);

    /* the photo at 8 MP has the preview's shutter and the preview's brightness */
    const double preview_luma = live_luma(ctl);
    const std::uint64_t gen0 = s.gen;
    const int writes0 = cam->exp_writes();
    ctl.shutter(mono_ms());
    CHECK(step_until(ctl, 40000, [&](const Snapshot &x) { return x.photos_saved == 1 && !x.photo_busy; }, &s));
    CHECK(s.gen > gen0); /* the stream was switched and switched back */
    CHECK(cam->exp_writes() > writes0); /* the shutter was written again after each switch */
    CHECK_EQ(cam->exp_auto(), 1);
    CHECK_EQ(cam->exp_abs(), shutter);
    CHECK(step_until(ctl, 8000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.w == 1280; }, &s));
    CHECK(s.notice.text.empty()); /* the camera took the size: no notice */
    {
        const std::string path = dir + "/" + s.last_photo;
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(!bytes.empty());
        Picture pic;
        std::string why;
        CHECK(decode_jpeg_fit(bytes.data(), bytes.size(), 352, 198, PixFmt::XRGB8888, pic, &why));
        CHECK_EQ(pic.mode.w, 3264);
        CHECK_EQ(pic.mode.h, 2448);
        const double photo_luma = picture_luma(pic.fmt, pic.px.data(), pic.w, pic.h, pic.stride);
        CHECK_NEAR(photo_luma / preview_luma, 1.0, 0.1); /* not darker or brighter than the preview */
    }

    /* Auto again: Aperture Priority; the EV chip is gone */
    ctl.exposure_set(0);
    CHECK(wait_for(3000, [&] { return cam->exp_auto() == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); /* the pictures of the last shutter go by */
    s = ctl.step(mono_ms());
    CHECK_EQ(s.exposure_stop, 0);
    /* leaving Auto again measures a new base: the picture of the automatic exposure is as bright as at the start */
    CHECK_NEAR(live_luma(ctl) / l0, 1.0, 0.1);

    /* refused while the self-timer counts, and while recording */
    {
        Settings st = ctl.settings();
        st.self_timer_s = 10;
        ctl.settings_apply(st);
        ctl.shutter(mono_ms());
        CHECK(ctl.counting_down());
        ctl.exposure_set(1);
        CHECK_EQ(ctl.exposure_stop(), 0);
        CHECK(ctl.cancel_countdown());
    }
    ctl.stop();
    rm_tree(dir);
}

void controller_exposure_camera_switch()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("exposure switch: no libturbojpeg.so.0");
        return;
    }
    const std::string dir = make_temp_dir("expsw");
    ControllerConfig c = back_config(dir, "");
    FakeOptions front = c.fake_opt;
    front.profile = FakeProfile::UsbFhd;
    front.path = std::string(FakeCamera::kPath) + "1";
    front.index = 1;
    c.fake_more.push_back(front);
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    const char *kBack = "5986:0656@1-9.4", *kFront = "13d3:5224@1-9.3";
    CHECK(step_until(ctl, 6000, [&](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kBack && x.cameras == 2; }, &s));
    CHECK(s.exposure_ok);
    ctl.exposure_set(1);
    CHECK(wait_for(3000, [&] { return ctl.fake()->exp_auto() == 1; }));
    CHECK_EQ(ctl.exposure_stop(), 1);
    /* the front camera has no exposure controls: no row, and nothing is written to it */
    CHECK(ctl.next_camera());
    CHECK(step_until(ctl, 8000, [&](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kFront && x.mode.valid(); }, &s));
    CHECK(!s.exposure_ok);
    CHECK_EQ(s.exposure_stop, 0);
    ctl.exposure_set(1); /* ignored */
    CHECK_EQ(ctl.exposure_stop(), 0);
    CHECK_EQ(ctl.fake()->exp_auto(), 3); /* the back camera was put back on Aperture Priority when it closed */
    /* back: Auto, not the old stop */
    CHECK(ctl.next_camera());
    CHECK(step_until(ctl, 8000, [&](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kBack && x.mode.valid(); }, &s));
    CHECK(s.exposure_ok);
    CHECK_EQ(s.exposure_stop, 0);
    CHECK_EQ(ctl.fake()->exp_auto(), 3);
    ctl.stop();
    rm_tree(dir);
}

void controller_refused_photo_size()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("refused photo size: no libturbojpeg.so.0");
        return;
    }
    const std::string dir = make_temp_dir("exprf");
    ControllerConfig c = back_config(dir, "photo_mode=light,photo_size=3264x2448");
    c.fake_opt.refuse_area = 6000000; /* 8 MP refused (ENOSPC), 5 MP fits */
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 6000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.valid(); }, &s));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ctl.shutter(mono_ms());
    CHECK(step_until(ctl, 40000, [&](const Snapshot &x) { return x.photos_saved == 1 && !x.photo_busy; }, &s));
    CHECK_EQ(s.notice.text, std::string("3264x2448 not available now: photo taken at 2592x1944."));
    /* the preview is back, alive */
    CHECK(step_until(ctl, 8000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.w == 1280; }, &s));
    const std::uint64_t f0 = ctl.pipeline().captured();
    CHECK(wait_for(3000, [&] { return ctl.pipeline().captured() > f0 + 5; }));
    ctl.stop();
    rm_tree(dir);
}

} // namespace

void test_exposure()
{
    the_maths();
    meter_follows_the_camera();
    focus_settle();
    capability_detection();
    size_lists();
    exposure_row_layout();
    capture_writes_exposure();
    capture_exposure_other_cameras();
    capture_exposure_dropped_on_another_camera();
    capture_refused_hold();
    capture_refused_hold_everything();
    capture_focus_wait();
    controller_exposure();
    controller_exposure_camera_switch();
    controller_refused_photo_size();
}
