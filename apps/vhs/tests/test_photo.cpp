/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: taking photos on the fake camera. The capture thread's photo slot (the next good frame after the warm-up,
 * of the mode asked for; a loss ends it with an error) and its mode hold (switch, release back); the controller's photo:
 * Instant (the next frame, the flash, the file with its EXIF), Light (the stream switches to the Photo size and back,
 * within the brief's 2 s), the self-timer (the countdown, the switch at its start in Light, a cancel takes nothing), a
 * camera without MJPEG refused, the review (list newest first, move, delete).
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "camera/capture.hpp"
#include "camera/fake_camera.hpp"
#include "jpeg/turbojpeg.hpp"
#include "live/clock.hpp"
#include "modes/select.hpp"
#include "scenes/fileops.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>

using namespace vhs;

namespace {

struct Rig {
    Rig() : cam(opts()), finder(make_fake_finder(cam)) {}
    static FakeOptions opts()
    {
        FakeOptions o;
        o.frames = 2;
        return o;
    }
    CaptureConfig config()
    {
        CaptureConfig c;
        c.api = &cam;
        c.finder = finder.get();
        c.poll_ms = 50;
        c.retry_ms = 100;
        c.lost_retry_ms = 50;
        c.choose = [](const std::vector<CamMode> &listed, const CamIdentity &) {
            SelectInput in;
            in.listed = listed;
            in.soc = SocClass::Other; /* Light: the preview at 720p */
            return select_modes(in).preview;
        };
        return c;
    }
    FakeCamera cam;
    std::unique_ptr<CameraFinder> finder;
};

void slot_and_hold()
{
    Rig r;
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(3000, [&] { return cap.status().state == CamState::Streaming; }));
    CHECK_EQ(mode_arg(cap.status().mode), std::string("MJPG:1280x720:30"));
    /* any mode: the next good frame after the warm-up (15 frames from STREAMON: about 0.5 s) */
    const std::int64_t t0 = mono_ms();
    cap.request_photo(ModeReq(), 7);
    PhotoShot shot;
    CHECK(wait_for(3000, [&] { return cap.take_photo(shot); }));
    CHECK(shot.ok);
    CHECK_EQ(shot.id, 7ull);
    CHECK_EQ(shot.mode.w, 1280);
    CHECK(!shot.data.empty());
    CHECK(mono_ms() - t0 >= 15 * 30); /* not before the warm-up after this STREAMON */
    CHECK(!cap.take_photo(shot)); /* once */
    /* now warmed up: the next frame, quickly */
    const std::int64_t t1 = mono_ms();
    cap.request_photo(ModeReq(), 8);
    CHECK(wait_for(3000, [&] { return cap.take_photo(shot); }));
    CHECK(mono_ms() - t1 < 300); /* the brief's Instant budget is 0.3 s from press to file */
    /* the hold: the Photo size streams, the photo of that mode, then back */
    ModeReq photo;
    parse_mode("MJPG:1920x1080:30", photo);
    const std::uint64_t gen = cap.status().gen;
    cap.hold_mode(photo);
    CHECK(cap.holding());
    cap.request_photo(photo, 9);
    CHECK(wait_for(4000, [&] { return cap.take_photo(shot); }));
    CHECK(shot.ok);
    CHECK_EQ(shot.mode.w, 1920);
    CHECK(cap.status().gen > gen);
    cap.release_hold();
    CHECK(!cap.holding());
    CHECK(wait_for(3000, [&] { return cap.status().mode.w == 1280; })); /* back to the preview */
    /* holding the mode that streams: no reopen */
    const std::uint64_t g2 = cap.status().gen;
    ModeReq cur;
    parse_mode("MJPG:1280x720:30", cur);
    cap.hold_mode(cur);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(cap.status().gen, g2);
    cap.release_hold();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(cap.status().gen, g2);
    /* a loss ends a request with an error */
    ModeReq never;
    parse_mode("MJPG:640x360:30", never); /* not a FineCam mode: never satisfied */
    cap.request_photo(never, 10);
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return cap.take_photo(shot); }));
    CHECK(!shot.ok);
    CHECK_EQ(shot.id, 10ull);
    CHECK(!shot.error.empty());
    r.cam.replug();
    /* cancel: nothing arrives */
    CHECK(wait_for(3000, [&] { return cap.status().state == CamState::Streaming; }));
    cap.request_photo(never, 11);
    cap.cancel_photo();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!cap.take_photo(shot));
    cap.stop();
}

ControllerConfig fake_config(const std::string &folder, const char *soc)
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = soc;
    c.clock_synced = 0;
    c.utc = true;
    return c;
}

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

std::vector<std::uint8_t> read_file(const std::string &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void instant()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("controller photo: no libturbojpeg.so.0 (the fake camera's MJPEG)");
        return;
    }
    const std::string dir = make_temp_dir("shot");
    Controller ctl(fake_config(dir + "/VHS", "bcm2837"));
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); /* warmed up */
    const std::int64_t t0 = mono_ms();
    ctl.shutter(mono_ms());
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.photos_saved == 1; }, &s));
    CHECK(mono_ms() - t0 < 600); /* press to file (the brief's budget is 0.3 s on a board; the fake runs at 30 fps) */
    CHECK_EQ(s.flash, 1ull);
    CHECK(!s.photo_busy);
    CHECK_EQ(s.last_photo, std::string("photo_001_nodate.jpg"));
    const std::vector<std::uint8_t> f = read_file(dir + "/VHS/photo_001_nodate.jpg");
    ExifFields e;
    CHECK(exif_parse(f.data(), f.size(), e));
    CHECK_EQ(e.make, std::string("Ingenic Semiconductor Co.,Ltd"));
    CHECK_EQ(e.model, std::string("UGREEN FineCam 1080P CM969"));
    CHECK_EQ(e.software, std::string("VHS 0.1.0"));
    CHECK_EQ(e.width, 1920u); /* Instant: the Photo size streams */
    CHECK_EQ(e.height, 1080u);
    CHECK(e.datetime.empty()); /* the clock is not synchronised (forced): no date */
    /* in Video mode the shutter takes no photo (recording is M3) */
    ctl.toggle_capture_mode();
    ctl.shutter(mono_ms());
    CHECK(!ctl.step(mono_ms()).photo_busy);
    ctl.toggle_capture_mode();
    /* the review: newest first, move, delete */
    ctl.shutter(mono_ms());
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.photos_saved == 2; }));
    ctl.stop(); /* the camera is released while the review shows */
    ctl.review_open(352, 198);
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) { return ctl.review().count == 2 && ctl.review_picture(); }));
    ReviewView v = ctl.review();
    CHECK_EQ(v.index, 0);
    CHECK_EQ(v.name, std::string("photo_002_nodate.jpg"));
    CHECK(v.meta.compare(0, 11, "1920x1080, ") == 0);
    CHECK(ctl.review_picture()->w == 352 && ctl.review_picture()->h == 198);
    const std::uint64_t pg = v.picture_gen;
    ctl.review_move(1);
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) { return ctl.review().picture_gen > pg; }));
    CHECK_EQ(ctl.review().name, std::string("photo_001_nodate.jpg"));
    ctl.review_move(1); /* the last: stays */
    CHECK_EQ(ctl.review().index, 1);
    ctl.review_delete();
    CHECK(step_until(ctl, 3000, [&](const Snapshot &) { return ctl.review().count == 1 && !ctl.review().loading; }));
    CHECK_EQ(ctl.review().index, 0);
    CHECK_EQ(ctl.review().name, std::string("photo_002_nodate.jpg"));
    ctl.review_close();
    CHECK(!ctl.review().open);
    /* the next number does not repeat the deleted one */
    ctl.start();
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ctl.shutter(mono_ms());
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.photos_saved == 3; }, &s));
    CHECK_EQ(s.last_photo, std::string("photo_003_nodate.jpg"));
    ctl.stop();
    rm_tree(dir);
}

void light_and_timer()
{
    if (!TurboJpeg::get().can_encode()) {
        SKIP("Light photo: no libturbojpeg.so.0");
        return;
    }
    const std::string dir = make_temp_dir("light");
    Controller ctl(fake_config(dir, "other")); /* Light by SoC class */
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
    CHECK_EQ(s.mode.w, 1280); /* the Light mode streams */
    const std::int64_t t0 = mono_ms();
    ctl.shutter(mono_ms());
    bool saw_switch = false;
    CHECK(step_until(ctl, 4000, [&](const Snapshot &x) {
        saw_switch |= x.mode.w == 1920;
        return x.photos_saved == 1;
    }, &s));
    const std::int64_t took = mono_ms() - t0;
    CHECK(saw_switch);
    CHECK(took < 2000); /* B5: under 2 s */
    const std::vector<std::uint8_t> f = read_file(dir + "/" + s.last_photo);
    ExifFields e;
    CHECK(exif_parse(f.data(), f.size(), e));
    CHECK_EQ(e.width, 1920u); /* the Photo size, not the preview's 720p */
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.w == 1280; }));
    ctl.stop();

    /* the self-timer in Light: the Photo size streams from the countdown's start; zero takes the photo at once */
    ControllerConfig c = fake_config(dir, "other");
    c.settings_overrides = "self_timer_s=3";
    Controller t(c);
    t.start();
    CHECK(step_until(t, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    const std::int64_t s0 = mono_ms();
    t.shutter(mono_ms());
    CHECK(t.counting_down());
    s = t.step(mono_ms());
    CHECK_EQ(s.countdown, 3);
    CHECK_EQ(s.timer_total, 3);
    CHECK(step_until(t, 3000, [](const Snapshot &x) { return x.countdown == 1 && x.mode.w == 1920; }));
    CHECK(step_until(t, 4000, [](const Snapshot &x) { return x.photos_saved == 1; }, &s));
    CHECK(mono_ms() - s0 >= 3000);
    CHECK(mono_ms() - s0 < 3600); /* zero acts at once: the stream had switched */
    CHECK_EQ(s.countdown, 0);
    CHECK_EQ(s.flash, 1ull);
    CHECK(step_until(t, 3000, [](const Snapshot &x) { return x.mode.w == 1280; })); /* back */
    /* a cancel: nothing taken, the stream back */
    t.shutter(mono_ms());
    CHECK(step_until(t, 3000, [](const Snapshot &x) { return x.mode.w == 1920; }));
    CHECK(t.cancel_countdown());
    CHECK(!t.counting_down());
    CHECK(!t.cancel_countdown());
    CHECK(step_until(t, 3000, [](const Snapshot &x) { return x.mode.w == 1280; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    s = t.step(mono_ms());
    CHECK_EQ(s.photos_saved, 1);
    CHECK_EQ(s.flash, 1ull);
    /* the camera released (hide) during a countdown: it ends */
    t.shutter(mono_ms());
    CHECK(t.counting_down());
    t.stop();
    CHECK(!t.counting_down());
    rm_tree(dir);
}

void refused()
{
    const std::string dir = make_temp_dir("yuyv");
    ControllerConfig c = fake_config(dir, "bcm2837");
    c.fake_opt.profile = FakeProfile::YuyvOnly;
    Controller ctl(c);
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
    ctl.shutter(mono_ms());
    s = ctl.step(mono_ms());
    CHECK_EQ(s.notice.text, std::string(kNoticeNoMjpegPhoto));
    CHECK(!s.photo_busy);
    CHECK_EQ(s.flash, 0ull);
    ctl.stop();
    std::vector<std::string> names;
    posix_file_ops().list_dir(dir, names);
    CHECK(names.empty());
    rm_tree(dir);
}

} // namespace

void test_photo()
{
    slot_and_hold();
    instant();
    light_and_timer();
    refused();
}
