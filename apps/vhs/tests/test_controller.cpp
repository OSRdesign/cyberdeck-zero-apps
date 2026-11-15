/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the controller (core/app/controller.hpp) on the fake camera: the board's selection at open (an Instant
 * board streams the Photo size; Light elsewhere), the notices of section 5 ("Camera unplugged." once per loss; busy and
 * no access standing while they last; the power notice once a minute while the camera is open), the replug hint on a
 * dwc_otg bus when the camera is not back, the fake script, the camera remembered by identity in the settings file,
 * the Photo / Video mode, and stop / start releasing and reopening the camera.
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "live/clock.hpp"
#include "scenes/fileops.hpp"
#include "settings/settings.hpp"

#include <fstream>
#include <iterator>

using namespace vhs;

namespace {

ControllerConfig fake_config()
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    return c;
}

/* steps the controller (as the page's timer) until pred(snapshot) or ms pass */
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

void selection_and_toggle()
{
    ControllerConfig c = fake_config();
    c.soc_override = "bcm2837";
    Controller ctl(c);
    CHECK(ctl.soc() == SocClass::Bcm2837);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1920x1080:30")); /* Instant: the Photo size */
    CHECK_EQ(s.camera, std::string("UGREEN FineCam 1080P CM969"));
    CHECK(!s.yuyv);
    CHECK(s.notice.text.empty());
    CHECK(ctl.selection().ok);
    CHECK(ctl.capture_mode() == CaptureMode::Photo);
    ctl.toggle_capture_mode();
    CHECK(ctl.step(mono_ms()).capture_mode == CaptureMode::Video);
    ctl.toggle_capture_mode();
    CHECK(ctl.capture_mode() == CaptureMode::Photo);
    /* stop releases the camera, start opens it again */
    ctl.stop();
    CHECK(!ctl.running());
    CHECK(!ctl.fake()->is_open());
    ctl.start();
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.gen >= 2; }));
    ctl.stop();

    ControllerConfig l = fake_config();
    l.soc_override = "other";
    Controller light(l);
    light.start();
    CHECK(step_until(light, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1280x720:30")); /* Light: the preview at 720p */
    light.stop();
}

void notices_and_script()
{
    ControllerConfig c = fake_config();
    c.fake_opt.hc_driver = "dwc_otg";
    c.fake_script = "unplug@600,replug@99999";
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    /* the script unplugs it: "Camera unplugged." once, the Lost state, then NotBack with the dwc_otg hint */
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Lost; }, &s));
    CHECK_EQ(s.notice.text, std::string(kNoticeUnplugged));
    CHECK(!s.replug_hint);
    CHECK(step_until(ctl, 8000, [](const Snapshot &x) { return x.state == CamState::NotBack; }, &s));
    CHECK(s.replug_hint);
    ctl.fake()->replug();
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK(!s.replug_hint);
    ctl.stop();
}

void standing_and_power()
{
    ControllerConfig c = fake_config();
    c.fake_script = "busy@0,free@1500";
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Busy; }, &s));
    CHECK_EQ(s.notice.text, std::string(kNoticeBusy));
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK(s.notice.text.empty()); /* gone with its cause */
    ctl.stop();

    ControllerConfig n = fake_config();
    n.fake_script = "noaccess@0";
    Controller na(n);
    na.start();
    CHECK(step_until(na, 3000, [](const Snapshot &x) { return x.notice.text == kNoticeNoAccess; }, &s));
    CHECK(s.state == CamState::NoAccess);
    na.stop();

    ControllerConfig p = fake_config();
    p.force_undervolt = true; /* as the firmware's bit 0 would say */
    Controller pw(p);
    pw.start();
    CHECK(step_until(pw, 4000, [](const Snapshot &x) { return x.notice.text == kNoticePower; }, &s));
    pw.stop();
}

void remembers_camera()
{
    const std::string dir = make_temp_dir("ctl");
    ControllerConfig c = fake_config();
    c.persist = true;
    c.settings_path = dir + "/settings.json";
    {
        Controller ctl(c);
        ctl.start();
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
        ctl.step(mono_ms());
        ctl.stop();
        CHECK_EQ(ctl.settings().camera_id, std::string("a108:2240:FAKE0001"));
    }
    std::ifstream f(dir + "/settings.json");
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("a108:2240:FAKE0001") != std::string::npos);
    /* a stored Photo size the camera lists: streamed on an Instant board */
    write_file(dir + "/settings.json", R"({"photo_size":"1280x960","photo_mode":"instant"})");
    c.soc_override = "other";
    Controller ctl2(c);
    ctl2.start();
    Snapshot s;
    CHECK(step_until(ctl2, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1280x960:30"));
    ctl2.stop();
    rm_tree(dir);
}

void yuyv_camera()
{
    ControllerConfig c = fake_config();
    c.fake_opt.profile = FakeProfile::YuyvOnly;
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK(s.yuyv);
    CHECK_EQ(mode_arg(s.mode), std::string("YUYV:640x480:30"));
    CHECK(wait_for(3000, [&] { return ctl.pipeline().pictures().take(); }));
    ctl.stop();
}

} // namespace

void test_controller()
{
    selection_and_toggle();
    notices_and_script();
    standing_and_power();
    remembers_camera();
    yuyv_camera();
}
