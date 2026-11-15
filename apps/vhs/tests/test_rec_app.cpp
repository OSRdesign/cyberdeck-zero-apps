/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the camera microphone and the controller's recording (brief 5.4, 6.4, 6.6, 6.7, 6.8, 13): mic matching on
 * sysfs fixture trees (the FineCam's CM969 on the camera's USB device, the C270's separate card, no microphone, a
 * headset on another port), the fake microphone through Cassette's open policy (hw: only: busy is refused) and the
 * audio capture with the lamp and the silence detector, then the controller with the fake camera and the fake
 * microphone on the build box's FFmpeg: a scene with sound, the stop paths (R, stop() as hide / close / SIGTERM's
 * teardown, the camera unplugged, the disk guard's refusal), a busy microphone (video only + toast), the video
 * self-timer and a camera without MJPEG.
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "audio/alsa/audio_capture.hpp"
#include "audio/alsa/open_policy.hpp"
#include "audio/fake_alsa.hpp"
#include "audio/level.hpp"
#include "audio/mic_match.hpp"
#include "camera/identity.hpp"
#include "jpeg/turbojpeg.hpp"
#include "live/clock.hpp"
#include "record/av_libs.hpp"
#include "record/journal.hpp"
#include "scenes/fileops.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace vhs;

namespace {

void link(const std::string &target, const std::string &at)
{
    make_dirs(at.substr(0, at.find_last_of('/')));
    CHECK(symlink(target.c_str(), at.c_str()) == 0);
}

/* a USB device dir with idVendor; its interfaces <dev>:1.N */
std::string usb_dev(const std::string &root, const std::string &dev)
{
    const std::string d = root + "/sys/devices/platform/xhci/usb1/1-1/" + dev;
    make_dirs(d);
    write_file(root + "/sys/devices/platform/xhci/usb1/1-1/idVendor", "1d6b\n");
    write_file(d + "/idVendor", "0bda\n");
    return d;
}

void video_node(const std::string &root, const std::string &devdir, const std::string &dev, const std::string &video)
{
    make_dirs(devdir + "/" + dev + ":1.0/video4linux/" + video);
    link(devdir + "/" + dev + ":1.0", root + "/sys/class/video4linux/" + video + "/device");
}

void sound_card(const std::string &root, const std::string &devdir, const std::string &intf, int n, const std::string &id)
{
    make_dirs(devdir + "/" + intf + "/sound/card" + std::to_string(n));
    link(devdir + "/" + intf, root + "/sys/class/sound/card" + std::to_string(n) + "/device");
    make_dirs(root + "/proc/asound/card" + std::to_string(n));
    write_file(root + "/proc/asound/card" + std::to_string(n) + "/id", id + "\n");
}

void mic_match()
{
    const std::string root = make_temp_dir("mic");
    /* the FineCam on 1-1.2: video on :1.0, its CM969 microphone on :1.2 */
    const std::string fine = usb_dev(root, "1-1.2");
    video_node(root, fine, "1-1.2", "video0");
    sound_card(root, fine, "1-1.2:1.2", 2, "CM969");
    /* the C270 on 1-1.3: its microphone is a separate card ("USB Audio") on :1.2 */
    const std::string c270 = usb_dev(root, "1-1.3");
    video_node(root, c270, "1-1.3", "video2");
    sound_card(root, c270, "1-1.3:1.2", 1, "U0x46d0x825");
    /* a headset on 1-1.4 (not a camera's) and a camera without a microphone on 1-1.5 */
    const std::string head = usb_dev(root, "1-1.4");
    sound_card(root, head, "1-1.4:1.0", 3, "Headset");
    const std::string bare = usb_dev(root, "1-1.5");
    video_node(root, bare, "1-1.5", "video4");
    /* the board's own audio: not USB */
    make_dirs(root + "/sys/devices/platform/soc/bcm2835_audio/sound/card0");
    link(root + "/sys/devices/platform/soc/bcm2835_audio", root + "/sys/class/sound/card0/device");

    MicCard m = camera_mic(root, usb_device_dir(root, "video0"));
    CHECK(m.found);
    CHECK_EQ(m.card.id, std::string("CM969"));
    CHECK_EQ(m.card.index, 2);
    CHECK(m.card.capture);
    CHECK_EQ(pcm_device(m.card), std::string("hw:CARD=CM969,DEV=0")); /* by id, never by index */
    m = camera_mic(root, usb_device_dir(root, "video2"));
    CHECK(m.found);
    CHECK_EQ(m.card.id, std::string("U0x46d0x825"));
    m = camera_mic(root, usb_device_dir(root, "video4"));
    CHECK(!m.found);
    m = camera_mic(root, "");
    CHECK(!m.found);
    m = camera_mic(root + "/nothing", usb_device_dir(root, "video0"));
    CHECK(!m.found);
    /* without sysfs (the fake camera): the card on the camera's USB port */
    FakeAlsa fa;
    m = camera_mic_by_port(fa, "1-1.2");
    CHECK(m.found && m.card.id == "CM969");
    CHECK(!camera_mic_by_port(fa, "1-1.3").found);
    FakeAlsa none(fake_mic_options("none"));
    CHECK(!camera_mic_by_port(none, "1-1.2").found);
    rm_tree(root);
}

struct CountSink : FrameSink {
    std::atomic<std::uint64_t> frames{0};
    std::size_t write(const void *f, std::size_t n) override
    {
        (void)f;
        frames += n;
        return n;
    }
};

void fake_microphone()
{
    /* hw: only (allow_server false): busy is refused, the rate is the card's own (16000 on the CM969) */
    FakeAlsa fa;
    CardInfo card = fa.cards().at(0);
    OpenOptions oo;
    oo.allow_server = false;
    oo.want_bits = 16;
    OpenResult r = open_capture(fa, card, oo);
    CHECK(r.ok());
    CHECK_EQ(r.format.rate, 16000u);
    CHECK_EQ(static_cast<int>(r.format.channels), 2);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    CHECK(!r.via_server);
    CHECK_EQ(r.device, std::string("hw:CARD=CM969,DEV=0"));
    FakeAlsa busy(fake_mic_options("busy"));
    OpenResult b = open_capture(busy, card, oo);
    CHECK(b.status == OpenStatus::Busy);
    CHECK(!b.via_server);

    /* the capture: the lamp lights on the tone; the clock pairs come; silence gives its event */
    FakeMicOptions to = fake_mic_options("tone");
    to.pace = false;
    FakeAlsa tone(to);
    OpenResult t = open_capture(tone, card, oo);
    CHECK(t.ok());
    CountSink sink;
    SoundLevel lvl;
    lvl.reset(16000, 2);
    std::atomic<int> clocks{0};
    CaptureOptions co;
    co.on_period = [&lvl](const void *f, size_t n, const AudioFormat &) { lvl.feed(static_cast<const std::int16_t *>(f), n); };
    co.on_clock = [&clocks](std::int64_t ns, std::uint64_t pos) {
        if (ns > 0 && pos > 0) ++clocks;
    };
    co.clock_every_s = 0.0;
    {
        AudioCapture cap(std::move(t), sink, co);
        CHECK(cap.start());
        CHECK(wait_for(2000, [&] { return lvl.lamp() && sink.frames.load() > 16000 && clocks.load() > 2; }));
        cap.stop();
    }
    CHECK(lvl.lamp());
    CHECK(!lvl.silence_seen());
    FakeMicOptions so = fake_mic_options("silent");
    so.pace = false;
    FakeAlsa silent(so);
    OpenResult s = open_capture(silent, card, oo);
    SoundLevel quiet;
    quiet.reset(16000, 2);
    co.on_period = [&quiet](const void *f, size_t n, const AudioFormat &) { quiet.feed(static_cast<const std::int16_t *>(f), n); };
    {
        CountSink s2;
        AudioCapture cap(std::move(s), s2, co);
        CHECK(cap.start());
        CHECK(wait_for(2000, [&] { return quiet.silence_seen(); }));
        cap.stop();
    }
    CHECK(quiet.silence_event());
    CHECK(!quiet.lamp());
    /* unplugged: the capture stops by itself */
    FakeMicOptions lo = fake_mic_options("tone");
    lo.lost_after_ms = 200;
    FakeAlsa lost(lo);
    OpenResult l = open_capture(lost, card, oo);
    {
        CountSink s3;
        AudioCapture cap(std::move(l), s3, CaptureOptions());
        CHECK(cap.start());
        CHECK(wait_for(2000, [&] { return !cap.running(); }));
        CHECK(cap.stats().stop_reason == CaptureError::Unplugged);
    }
}

/* ------------------------------------------------------------------------------------------------ the controller */

ControllerConfig rec_config(const std::string &folder, const std::string &mic, const std::string &set = "")
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = "bcm2837"; /* Instant: the preview streams 1920x1080, a scene switches to 1280x720 */
    c.clock_synced = 0;
    c.utc = true;
    c.fake_mic = mic;
    c.settings_overrides = set;
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

bool streaming(Controller &ctl)
{
    if (!step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; })) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(600)); /* warmed up */
    return true;
}

bool exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/* the streams of a finished scene, read back in-process */
int streams_of(const AvLibs &av, const std::string &file)
{
    av::FormatContext *ic = nullptr;
    if (av.open_input(&ic, file.c_str(), nullptr, nullptr) < 0) return -1;
    av.find_stream_info(ic, nullptr);
    const int n = static_cast<int>(av.fmt_nb_streams(ic));
    av.close_input(&ic);
    return n;
}

bool saw_notice(Controller &ctl, const std::string &text, int ms)
{
    return step_until(ctl, ms, [&](const Snapshot &x) { return x.notice.text == text; });
}

void controller_recording()
{
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (!av) {
        SKIP(("controller recording: no FFmpeg libraries (" + why + ")").c_str());
        return;
    }
    if (!TurboJpeg::get().can_encode()) {
        SKIP("controller recording: no libturbojpeg.so.0 (the fake camera's MJPEG)");
        return;
    }
    const std::string dir = make_temp_dir("crec");

    /* 1. R starts a scene with sound (1920x1080 preview: the 1280x720 Video size is held), R stops it */
    {
        Controller ctl(rec_config(dir + "/a", "tone"));
        ctl.start();
        CHECK(streaming(ctl));
        Snapshot s = ctl.step(mono_ms());
        CHECK(s.mode.w == 1920); /* the preview (Instant on bcm2837) */
        const std::int64_t t0 = mono_ms();
        ctl.record(mono_ms());
        CHECK(ctl.recording());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started; }, &s));
        const std::int64_t first = mono_ms() - t0;
        CHECK(first <= 1500); /* brief 14: REC to the first written frame with a mode switch */
        CHECK(s.recording && s.rec_audio);
        CHECK_EQ(s.rec_mode.w, 1280);
        CHECK_EQ(s.rec_mode.h, 720);
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.lamp && x.rec_ms >= 1500; }, &s));
        CHECK(s.mode.w == 1280); /* the Video size streams */
        CHECK(s.rec_bytes > 0);
        /* the shutter in Photo mode stops too (Space, the red button); "Photo now" before that */
        ctl.photo_now(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.photos_saved == 1; }, &s));
        CHECK(s.recording);
        CHECK_EQ(s.last_photo, std::string("photo_002_nodate.jpg")); /* the scene took 001 */
        ctl.shutter(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }, &s));
        CHECK_EQ(s.scenes_recorded, 1);
        CHECK_EQ(s.last_scene, std::string("scene_001_nodate.mkv"));
        const RecorderStatus st = ctl.last_recording();
        CHECK(st.stop == RecStop::User);
        CHECK(st.audio);
        CHECK(st.frames >= 40);
        CHECK_EQ(st.ring_drops, 0ull);
        CHECK(exists(dir + "/a/scene_001_nodate.mkv"));
        CHECK_EQ(streams_of(*av, dir + "/a/scene_001_nodate.mkv"), 2);
        Journal j;
        CHECK(journal_read(posix_file_ops(), journal_path(dir + "/a", "scene_001_nodate"), j));
        CHECK_EQ(j.state, std::string("closed"));
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.mode.w == 1920; })); /* the hold released */
        std::fprintf(stderr, "controller: REC to the first written frame %lld ms (with the switch to 1280x720), %llu frames\n",
                     static_cast<long long>(first), static_cast<unsigned long long>(st.frames));
        /* 2. stop() while recording (hide, close, SIGTERM's teardown): the file is finished before it returns */
        ctl.toggle_capture_mode(); /* Video mode: the shutter records; the Video size streams (owner, 2026-11-15) */
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.mode.w == 1280; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(700)); /* warmed up */
        const std::int64_t tv = mono_ms();
        ctl.shutter(mono_ms());
        CHECK(step_until(ctl, 2000, [](const Snapshot &x) { return x.rec_started; }));
        const std::int64_t fast = mono_ms() - tv;
        CHECK(fast <= 400); /* no stream switch: the warm-up is already done */
        std::fprintf(stderr, "controller: REC in Video mode to the first written frame %lld ms (no switch)\n",
                     static_cast<long long>(fast));
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started && x.rec_ms > 500; }));
        ctl.stop();
        CHECK(!ctl.recording());
        CHECK(ctl.last_recording().stop == RecStop::Released);
        CHECK(ctl.last_recording().finished);
        Journal j2;
        CHECK(journal_read(posix_file_ops(), journal_path(dir + "/a", "scene_003_nodate"), j2));
        CHECK_EQ(j2.state, std::string("closed"));
        CHECK_EQ(streams_of(*av, dir + "/a/scene_003_nodate.mkv"), 2);
    }

    /* 3. the camera unplugged while recording: the scene ends at the last frame, "Camera unplugged." */
    {
        Controller ctl(rec_config(dir + "/b", "tone"));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started && x.rec_ms > 800; }));
        ctl.fake()->unplug();
        Snapshot s;
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }, &s));
        CHECK(ctl.last_recording().stop == RecStop::CameraLost);
        CHECK(ctl.last_recording().frames > 10);
        CHECK(saw_notice(ctl, kNoticeUnplugged, 1000) || s.notice.text == kNoticeUnplugged);
        Journal j;
        CHECK(journal_read(posix_file_ops(), journal_path(dir + "/b", "scene_001_nodate"), j));
        CHECK_EQ(j.state, std::string("closed"));
        ctl.fake()->replug();
        ctl.stop();
    }

    /* 4. the microphone busy: video only, the toast */
    {
        Controller ctl(rec_config(dir + "/c", "busy"));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        Snapshot s;
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started; }, &s));
        CHECK(!s.rec_audio);
        CHECK(!s.lamp);
        CHECK(saw_notice(ctl, kNoticeMicBusy, 1000) || s.notice.text == kNoticeMicBusy);
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }));
        CHECK(!ctl.last_recording().audio);
        CHECK_EQ(streams_of(*av, dir + "/c/scene_001_nodate.mkv"), 1);
        ctl.stop();
    }

    /* 5. no microphone on the camera: video only, the toast; silence: its toast once */
    {
        Controller ctl(rec_config(dir + "/d", "none"));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        CHECK(saw_notice(ctl, kNoticeNoMic, 1500));
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }));
        ctl.stop();
        Controller sil(rec_config(dir + "/e", "silent"));
        sil.start();
        CHECK(streaming(sil));
        sil.record(mono_ms());
        CHECK(saw_notice(sil, kNoticeNoSound, 5000)); /* 3 s of exact zeros */
        sil.stop();
        CHECK(sil.last_recording().audio);
    }

    /* 6. the disk floor refuses REC: the red toast, nothing recorded */
    {
        ControllerConfig c = rec_config(dir + "/f", "tone");
        c.fake_free_mb = 200; /* under the default floor of 500 MB */
        Controller ctl(c);
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        CHECK(!ctl.recording());
        Snapshot s = ctl.step(mono_ms());
        CHECK_EQ(s.notice.text, std::string(kNoticeDiskFull));
        CHECK(s.notice.red);
        ctl.stop();
        CHECK(!exists(dir + "/f/scene_001_nodate.mkv"));
    }

    /* 7. the video self-timer: the Video size streams from the countdown's start, recording starts at zero; Esc cancels */
    {
        Controller ctl(rec_config(dir + "/g", "tone", "self_timer_s=3"));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        Snapshot s = ctl.step(mono_ms());
        CHECK(s.countdown == 3 && s.countdown_video);
        CHECK(!s.recording);
        CHECK(step_until(ctl, 2500, [](const Snapshot &x) { return x.mode.w == 1280; })); /* switched at the start */
        CHECK(ctl.counting_down());
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.recording; }, &s));
        const std::int64_t t0 = mono_ms();
        CHECK(s.countdown == 0);
        CHECK(step_until(ctl, 1500, [](const Snapshot &x) { return x.rec_started; }));
        std::fprintf(stderr, "controller: video self-timer: the first frame %lld ms after zero\n",
                     static_cast<long long>(mono_ms() - t0));
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }));
        /* cancelled: nothing recorded, the preview's mode back */
        ctl.record(mono_ms());
        CHECK(ctl.counting_down());
        CHECK(ctl.cancel_countdown());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.mode.w == 1920 && !x.recording; }));
        CHECK_EQ(ctl.step(mono_ms()).scenes_recorded, 1);
        ctl.stop();
    }

    /* 8. a camera without MJPEG cannot record */
    {
        ControllerConfig c = rec_config(dir + "/h", "tone");
        c.fake_opt.profile = FakeProfile::YuyvOnly;
        Controller ctl(c);
        ctl.start();
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; }));
        ctl.record(mono_ms());
        CHECK(!ctl.recording());
        CHECK_EQ(ctl.step(mono_ms()).notice.text, std::string(kNoticeNoMjpegVideo));
        ctl.stop();
    }
    rm_tree(dir);
}

} // namespace

void test_rec_app()
{
    mic_match();
    fake_microphone();
    controller_recording();
}
