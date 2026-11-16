/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the exposure_auto_priority rule (brief 5.3, C14) on the fake camera: present (read, set 0, restored at
 * close), absent (nothing touched), inactive and read-only (read, not set), the log line's words.
 */
#include "check.hpp"

#include "camera/controls.hpp"
#include "camera/fake_camera.hpp"

using namespace vhs;

namespace {

ExposurePriority run(FakeCtrl ctrl, FakeProfile profile)
{
    FakeOptions o;
    o.profile = profile;
    o.realtime = false;
    o.ctrl = static_cast<int>(ctrl);
    FakeCamera cam(o);
    const int fd = cam.open(FakeCamera::kPath);
    const ExposurePriority e = apply_exposure_priority(cam, fd);
    cam.close(fd);
    return e;
}

void rule()
{
    {
        FakeOptions o;
        o.profile = FakeProfile::C270;
        o.realtime = false;
        FakeCamera cam(o);
        const int fd = cam.open(FakeCamera::kPath);
        CHECK_EQ(cam.fps_now(), 15); /* the C270 left at 1 slows itself down indoors */
        const ExposurePriority e = apply_exposure_priority(cam, fd);
        CHECK(e.present && e.usable() && e.set);
        CHECK_EQ(e.was, 1);
        CHECK_EQ(cam.ctrl_value(), 0);
        CHECK_EQ(cam.ctrl_sets(), 1);
        CHECK_EQ(controls_log(e), std::string("controls: exposure_auto_priority was 1, set 0"));
        restore_exposure_priority(cam, fd, e);
        CHECK_EQ(cam.ctrl_value(), 1);
        /* already 0: set again (harmless), nothing to restore */
        cam.set_ctrl_value(0);
        const ExposurePriority z = apply_exposure_priority(cam, fd);
        CHECK_EQ(z.was, 0);
        const int sets = cam.ctrl_sets();
        restore_exposure_priority(cam, fd, z);
        CHECK_EQ(cam.ctrl_sets(), sets);
        cam.close(fd);
    }
    const ExposurePriority a = run(FakeCtrl::Absent, FakeProfile::FineCam);
    CHECK(!a.present && !a.set);
    CHECK_EQ(controls_log(a), std::string("controls: exposure_auto_priority absent"));
    const ExposurePriority i = run(FakeCtrl::Inactive, FakeProfile::C270);
    CHECK(i.present && i.inactive && !i.usable() && !i.set);
    CHECK_EQ(i.was, 1);
    CHECK_EQ(controls_log(i), std::string("controls: exposure_auto_priority inactive (was 1)"));
    const ExposurePriority r = run(FakeCtrl::ReadOnly, FakeProfile::C270);
    CHECK(r.present && r.read_only && !r.set);
    CHECK_EQ(controls_log(r), std::string("controls: exposure_auto_priority read-only (1)"));
    /* the FineCam with the control (B29 finds out whether it has one) */
    const ExposurePriority f = run(FakeCtrl::Present, FakeProfile::FineCam);
    CHECK(f.set);
    /* restore after the camera went away: best effort, no harm */
    FakeOptions o;
    o.profile = FakeProfile::C270;
    o.realtime = false;
    FakeCamera cam(o);
    const int fd = cam.open(FakeCamera::kPath);
    const ExposurePriority e = apply_exposure_priority(cam, fd);
    cam.unplug();
    restore_exposure_priority(cam, fd, e);
    CHECK_EQ(cam.ctrl_value(), 0);
}

} // namespace

void test_controls()
{
    rule();
}
