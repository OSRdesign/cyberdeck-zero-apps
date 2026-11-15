/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the capture thread (core/camera/capture.hpp) on the fake camera: it opens the chooser's mode, streams into
 * the mailbox, switches modes, and runs the states of brief 5.2 / 5.5: unplug (Lost, then NotBack after the hold), a
 * replug (back silently), a turn (the orientation flipped: Turned until the first frame, the portrait list then), a
 * different camera or a late return (no Turned), busy, no access, no camera, ten EIO in a row, the driver's sequence
 * gaps and error-flagged buffers; the exposure control rule at every open; the buffer count; and a prompt stop.
 */
#include "check.hpp"

#include "camera/capture.hpp"
#include "camera/fake_camera.hpp"
#include "live/clock.hpp"
#include "modes/select.hpp"

#include <chrono>
#include <memory>
#include <thread>

using namespace vhs;

namespace {

FakeOptions small()
{
    FakeOptions o;
    o.frames = 2;
    return o;
}

struct Rig {
    explicit Rig(FakeOptions o = small()) : cam(o), finder(make_fake_finder(cam)) {}
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
        /* the board's selection: the largest MJPEG mode (an Instant board) */
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

bool state_is(const Capture &cap, CamState s)
{
    return cap.status().state == s;
}

void streams_and_switches()
{
    Rig r;
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    CHECK(state_is(cap, CamState::Searching));
    cap.start();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    CHECK(wait_for(3000, [&] { return cap.frames() >= 5; }));
    CaptureStatus st = cap.status();
    CHECK_EQ(mode_arg(st.mode), std::string("MJPG:1920x1080:30")); /* the chooser's: Instant, the largest */
    CHECK_EQ(st.listed.size(), static_cast<std::size_t>(8));
    CHECK_EQ(st.card, r.cam.card());
    CHECK_EQ(st.id.key(), std::string("a108:2240:FAKE0001"));
    CHECK_EQ(st.bus_driver, std::string("xhci_hcd"));
    CHECK_EQ(st.gen, 1ull);
    CHECK_EQ(st.losses, 0ull);
    CHECK(box.take());
    const Frame &f = box.front();
    CHECK_EQ(f.mode.fourcc, kMJPG);
    CHECK(f.size > 0);
    CHECK(f.ts_us > 0);
    CHECK(f.ts_us <= mono_us());
    CHECK(f.dq_us >= f.ts_us);
    CHECK_EQ(f.gen, 1ull);
    CHECK(cap.tid() > 0);

    ModeReq yuyv;
    CHECK(parse_mode("YUYV:640x360:30", yuyv));
    cap.request_mode(yuyv);
    CHECK(wait_for(3000, [&] { return cap.status().mode.same(yuyv); }));
    CHECK(wait_for(3000, [&] { return box.take() && box.front().mode.fourcc == kYUYV; }));
    CHECK_EQ(box.front().stride, 1280);
    CHECK_EQ(box.front().size, static_cast<std::size_t>(640 * 360 * 2));
    CHECK_EQ(box.front().gen, 2ull);
    CHECK_EQ(cap.opens(), 2ull);
    CHECK_EQ(r.cam.streamon_count(), 2);
    /* reopen: the explicit mode stays */
    cap.reopen();
    CHECK(wait_for(3000, [&] { return cap.status().gen == 3 && cap.status().mode.same(yuyv); }));
    const std::int64_t t0 = mono_us();
    cap.stop();
    CHECK(mono_us() - t0 < 1000000);
    CHECK(!r.cam.is_open()); /* the device is closed once stop() returns */
    CHECK(state_is(cap, CamState::Streaming)); /* the last state stays readable */
}

void not_offered()
{
    Rig r;
    LatestMailbox<Frame> box;
    CaptureConfig c = r.config();
    CHECK(parse_mode("MJPG:640x360:30", c.want)); /* the CM969 has no MJPEG 640x360 */
    Capture cap(c, box);
    cap.start();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    const CaptureStatus st = cap.status();
    CHECK_EQ(mode_arg(st.mode), std::string("MJPG:1920x1080:30")); /* the chooser's instead */
    CHECK(st.detail.find("not offered") != std::string::npos);
    cap.stop();
}

void unplug_replug_notback()
{
    Rig r;
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
    CHECK_EQ(cap.status().losses, 1ull);
    CHECK(cap.status().lost_ms > 0);
    const std::uint64_t before = cap.frames();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(cap.frames(), before); /* nothing while it is away */
    CHECK(!cap.status().mode.valid());
    /* back quickly, the same camera, the same orientation: a replug, streaming again at once (no Turned) */
    r.cam.replug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    CHECK(wait_for(3000, [&] { return cap.frames() >= before + 3; }));
    CHECK_EQ(cap.status().gen, 2ull);
    CHECK_EQ(cap.status().turns, 0ull);
    CHECK_EQ(cap.status().lost_ms, 0);
    /* away longer than the hold: NotBack (the NoCamera page), the loss counted once */
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::NotBack); }));
    CHECK_EQ(cap.status().losses, 2ull);
    CHECK_EQ(cap.status().bus_driver, std::string("xhci_hcd")); /* remembered for the hint */
    r.cam.replug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    cap.stop();
}

void turn()
{
    FakeOptions o = small();
    o.restart_delay_ms = 400; /* the camera's first frame after re-enumerating */
    Rig r(o);
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
    CHECK_EQ(mode_arg(cap.status().mode), std::string("MJPG:1920x1080:30"));
    /* the switch moves: the camera drops and comes back with its portrait list */
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
    r.cam.set_portrait(true);
    r.cam.replug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Turned); }));
    CHECK_EQ(cap.status().turns, 1ull);
    CHECK_EQ(mode_arg(cap.status().mode), std::string("MJPG:1080x1920:30")); /* remapped: the portrait list */
    /* held until the first frame, then streaming */
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    CHECK(wait_for(3000, [&] { return box.take() && box.front().mode.h == 1920; }));
    /* turned back */
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
    r.cam.set_portrait(false);
    r.cam.replug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Turned); }));
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    CHECK_EQ(cap.status().turns, 2ull);
    CHECK_EQ(cap.status().mode.w, 1920);
    cap.stop();
}

void other_camera_or_late()
{
    /* another camera in the same port with the other orientation: a new camera, not a turn */
    {
        Rig r;
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
        r.cam.unplug();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
        CamIdentity id = r.cam.identity();
        id.serial = "OTHER";
        r.cam.set_identity(id);
        r.cam.set_portrait(true);
        r.cam.replug();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
        CHECK_EQ(cap.status().turns, 0ull);
        CHECK_EQ(cap.status().id.serial, std::string("OTHER"));
        cap.stop();
    }
    /* the same camera turned but back after the window: a fresh start */
    {
        Rig r;
        LatestMailbox<Frame> box;
        CaptureConfig c = r.config();
        c.back_window_ms = 150;
        Capture cap(c, box);
        cap.start();
        CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
        r.cam.unplug();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
        r.cam.set_portrait(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.cam.replug();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
        CHECK_EQ(cap.status().turns, 0ull);
        CHECK_EQ(cap.status().mode.h, 1920);
        cap.stop();
    }
}

void busy_noaccess_nocamera()
{
    {
        Rig r;
        r.cam.set_busy(true);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Busy); }));
        CHECK(!r.cam.is_open()); /* closed again, retried later */
        r.cam.set_busy(false);
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
        cap.stop();
    }
    {
        Rig r;
        r.cam.set_no_access(true);
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::NoAccess); }));
        r.cam.set_no_access(false);
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
        cap.stop();
    }
    {
        Rig r;
        r.cam.unplug(); /* never there */
        LatestMailbox<Frame> box;
        Capture cap(r.config(), box);
        cap.start();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::NoCamera); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(state_is(cap, CamState::NoCamera)); /* not Lost or NotBack: it never streamed */
        CHECK_EQ(cap.status().losses, 0ull);
        const std::int64_t t0 = mono_us();
        cap.stop(); /* while it sleeps between two looks */
        CHECK(mono_us() - t0 < 1000000);
    }
    {
        /* no finder at all */
        Rig r;
        LatestMailbox<Frame> box;
        CaptureConfig c = r.config();
        c.finder = nullptr;
        Capture cap(c, box);
        cap.start();
        CHECK(wait_for(3000, [&] { return state_is(cap, CamState::NoCamera); }));
        cap.stop();
    }
}

void eio_run()
{
    Rig r;
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
    r.cam.eio_every_dqbuf(true);
    /* ten EIO in a row count as a loss */
    CHECK(wait_for(3000, [&] { return cap.status().losses >= 1; }));
    CHECK(cap.errors() >= 10);
    r.cam.eio_every_dqbuf(false);
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming) && cap.status().gen >= 2; }));
    cap.stop();
}

void gaps_and_errors()
{
    Rig r;
    r.cam.skip_sequence_every(3);
    r.cam.error_flag_every(4);
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(4000, [&] { return cap.frames() >= 6 && cap.gaps() >= 2 && cap.errors() >= 1; }));
    CHECK(cap.gaps() >= 2);
    CHECK(cap.errors() >= 1);
    CHECK(state_is(cap, CamState::Streaming)); /* both are counted, not fatal */
    cap.stop();
}

void exposure_rule()
{
    /* the C270: present and 1 -> set 0 at open (30 fps), back to 1 at close; again after a replug */
    FakeOptions o = small();
    o.profile = FakeProfile::C270;
    Rig r(o);
    CHECK_EQ(r.cam.ctrl_value(), 1);
    LatestMailbox<Frame> box;
    Capture cap(r.config(), box);
    cap.start();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming); }));
    CHECK_EQ(mode_arg(cap.status().mode), std::string("MJPG:1280x960:30")); /* the C270's largest, no 1080p */
    const ExposurePriority e = cap.status().exposure;
    CHECK(e.present && e.set && e.was == 1);
    CHECK_EQ(r.cam.ctrl_value(), 0);
    CHECK_EQ(r.cam.fps_now(), 30);
    CHECK_EQ(controls_log(e), std::string("controls: exposure_auto_priority was 1, set 0"));
    /* unplugged and back: the camera forgot it (another app set it to 1): set again */
    r.cam.unplug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Lost); }));
    r.cam.set_ctrl_value(1);
    r.cam.replug();
    CHECK(wait_for(3000, [&] { return state_is(cap, CamState::Streaming) && cap.status().gen == 2; }));
    CHECK_EQ(r.cam.ctrl_value(), 0);
    cap.stop();
    CHECK_EQ(r.cam.ctrl_value(), 1); /* restored at close */
}

void buffers()
{
    Rig r;
    LatestMailbox<Frame> box;
    CaptureConfig c = r.config();
    c.buffers = 3; /* ram_mb <= 512 */
    Capture cap(c, box);
    cap.start();
    CHECK(wait_for(3000, [&] { return cap.frames() >= 3; }));
    cap.stop();
    c.buffers = 1; /* clamped to 2 */
    Capture cap2(c, box);
    cap2.start();
    CHECK(wait_for(3000, [&] { return cap2.frames() >= 3; }));
    cap2.stop();
}

} // namespace

void test_capture()
{
    streams_and_switches();
    not_offered();
    unplug_replug_notback();
    turn();
    other_camera_or_late();
    busy_noaccess_nocamera();
    eio_run();
    gaps_and_errors();
    exposure_rule();
    buffers();
}
