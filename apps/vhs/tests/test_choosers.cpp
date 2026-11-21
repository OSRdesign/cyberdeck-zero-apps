/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the camera and sound input choosers (0.1.2). The cameras: the chooser's rows and labels from candidates
 * (two names, the same name twice with the USB ports, front / back, #1 / #2), the stable id of a non-USB node, a scan of
 * the Panasonic FZ-M1's sysfs tree (two UVC cameras, each with a metadata node, no audio), the fallback rule, the layout of
 * the camera chip, the settings keys, and the controller on two fake cameras (the chip's data, the switch, the remembered
 * camera, the sizes per camera, the refusals). The sound input: the plan and the rows on the fake cards (the FZ-M1's
 * "HDA Intel PCH", mono, 32-bit only, busy behind a sound server, plughw only, dead), the 16-bit conversions, and a
 * scene recorded from the built-in card.
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "audio/alsa/audio_capture.hpp"
#include "audio/alsa/open_policy.hpp"
#include "audio/fake_alsa.hpp"
#include "audio/sound_input.hpp"
#include "camera/camera.hpp"
#include "camera/fake_camera.hpp"
#include "camera/identity.hpp"
#include "jpeg/turbojpeg.hpp"
#include "layout/capture_layout.hpp"
#include "live/clock.hpp"
#include "record/av_libs.hpp"
#include "scenes/fileops.hpp"
#include "settings/settings.hpp"

#include <linux/videodev2.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace vhs;

namespace {

Candidate cand(const std::string &product, const std::string &vid, const std::string &pid, const std::string &devpath,
               const std::string &serial = "", int orientation = -1, bool capture = true)
{
    Candidate c;
    c.by_id = true;
    c.capture = capture;
    c.mjpg = true;
    c.ref.id.vid = vid;
    c.ref.id.pid = pid;
    c.ref.id.serial = serial;
    c.ref.id.devpath = devpath;
    c.ref.id.product = product;
    c.ref.card = product + ": " + product;
    c.ref.path = "/dev/" + product;
    c.ref.name = product;
    c.ref.orientation = orientation;
    return c;
}

void entries_and_labels()
{
    /* the FZ-M1: two names, no hint needed; the card name "X: X" is cut to one X */
    {
        const std::vector<CameraEntry> e = camera_entries({cand("USB FHD Webcam", "13d3", "5224", "1-9.3"),
                                                           cand("USB2.0 Camera", "5986", "0656", "1-9.4")});
        CHECK_EQ(e.size(), 2u);
        CHECK_EQ(e[0].label, std::string("USB FHD Webcam"));
        CHECK_EQ(e[1].label, std::string("USB2.0 Camera"));
        CHECK_EQ(e[0].key, std::string("13d3:5224@1-9.3")); /* no serial: the USB port, never /dev/videoN */
        CHECK_EQ(e[1].key, std::string("5986:0656@1-9.4"));
        CHECK_EQ(e[0].port, std::string("9.3"));
    }
    CHECK_EQ(readable_camera_name("USB FHD Webcam: USB FHD Webcam"), std::string("USB FHD Webcam"));
    CHECK_EQ(readable_camera_name("Integrated Camera: Integrated C"), std::string("Integrated Camera: Integrated C"));
    CHECK_EQ(readable_camera_name("C270 HD WEBCAM"), std::string("C270 HD WEBCAM"));
    /* the same name twice: the USB connector when there are no ports' twins, "front" / "back" when the kernel says */
    {
        const std::vector<CameraEntry> e = camera_entries({cand("Webcam", "1111", "2222", "1-9.3"), cand("Webcam", "1111", "2222", "1-9.4")});
        CHECK_EQ(e[0].label, std::string("Webcam (USB 9.3)"));
        CHECK_EQ(e[1].label, std::string("Webcam (USB 9.4)"));
        CHECK(e[0].key != e[1].key);
    }
    {
        const std::vector<CameraEntry> e =
            camera_entries({cand("Webcam", "1111", "2222", "1-9.3", "", 0), cand("Webcam", "1111", "2222", "1-9.4", "", 1)});
        CHECK_EQ(e[0].label, std::string("Webcam (front)"));
        CHECK_EQ(e[1].label, std::string("Webcam (back)"));
    }
    { /* one with a location and one without: the location would leave a twin, so the ports name them */
        const std::vector<CameraEntry> e =
            camera_entries({cand("Webcam", "1111", "2222", "1-9.3", "", 0), cand("Webcam", "1111", "2222", "1-9.4", "", -1)});
        CHECK_EQ(e[0].label, std::string("Webcam (USB 9.3)"));
        CHECK_EQ(e[1].label, std::string("Webcam (USB 9.4)"));
    }
    { /* same name, same serial-less USB path text: "#1", "#2" */
        Candidate a = cand("Webcam", "1111", "2222", "");
        Candidate b = cand("Webcam", "1111", "2222", "");
        b.ref.id.serial = "B";
        a.ref.id.serial = "A";
        const std::vector<CameraEntry> e = camera_entries({a, b});
        CHECK_EQ(e[0].label, std::string("Webcam #1"));
        CHECK_EQ(e[1].label, std::string("Webcam #2"));
    }
    { /* a metadata node (no capture), a node without MJPG / YUYV and a second node of one device are not listed */
        Candidate meta = cand("USB FHD Webcam", "13d3", "5224", "1-9.3", "", -1, false);
        Candidate none = cand("Odd", "9999", "0001", "1-2");
        none.mjpg = false;
        const std::vector<CameraEntry> e =
            camera_entries({meta, cand("USB FHD Webcam", "13d3", "5224", "1-9.3"), cand("USB FHD Webcam", "13d3", "5224", "1-9.3"), none});
        CHECK_EQ(e.size(), 1u);
    }
    CHECK(camera_entries({}).empty());

    /* the pick and the fallback */
    const std::vector<Candidate> two = {cand("A", "1", "1", "1-1"), cand("B", "2", "2", "1-2")};
    CHECK_EQ(pick_camera(two, "2:2@1-2"), 1);
    CHECK_EQ(pick_camera(two, "9:9@1-9"), 0); /* not plugged in: the first */
    CHECK(camera_fell_back(two, "9:9@1-9"));
    CHECK(!camera_fell_back(two, "2:2@1-2"));
    CHECK(!camera_fell_back(two, ""));
    CHECK(!camera_fell_back({cand("A", "1", "1", "1-1")}, "9:9@1-9")); /* one camera: silent, as 0.1.1 */
    CHECK(!camera_fell_back({}, "9:9@1-9"));
}

/* ------------------------------------------------------------------------------------------------ sysfs fixtures */

void link(const std::string &target, const std::string &at)
{
    make_dirs(at.substr(0, at.find_last_of('/')));
    CHECK(symlink(target.c_str(), at.c_str()) == 0);
}

/* A UVC camera on the internal hub: the USB device (no serial), its video interface with the capture node `video` and the
 * metadata node video+1 (index 1), the uvcvideo driver link. */
void uvc_camera(const std::string &root, const std::string &devname, const std::string &vid, const std::string &pid,
                const std::string &product, int video)
{
    const std::string hub = root + "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-9";
    const std::string dev = hub + "/" + devname;
    const std::string intf = dev + "/" + devname + ":1.0";
    make_dirs(intf);
    write_file(hub + "/idVendor", "05e3\n");
    write_file(dev + "/idVendor", vid + "\n");
    write_file(dev + "/idProduct", pid + "\n");
    write_file(dev + "/product", product + "\n");
    make_dirs(root + "/sys/bus/usb/drivers/uvcvideo");
    link(root + "/sys/bus/usb/drivers/uvcvideo", intf + "/driver");
    for (int i = 0; i < 2; ++i) {
        const std::string v = "video" + std::to_string(video + i);
        link(intf, root + "/sys/class/video4linux/" + v + "/device");
        write_file(root + "/sys/class/video4linux/" + v + "/index", std::to_string(i) + "\n");
    }
}

void non_usb_identity()
{
    const std::string root = make_temp_dir("chid");
    const std::string plat = root + "/sys/devices/platform/soc/fe801000.csi";
    make_dirs(plat);
    link(plat, root + "/sys/class/video4linux/video0/device");
    const CamIdentity id = identity_for_node(root, "video0");
    CHECK(!id.valid());
    CHECK_EQ(id.stable, std::string("sys:platform/soc/fe801000.csi"));
    CHECK_EQ(id.stable_key(), std::string("sys:platform/soc/fe801000.csi"));
    CHECK(id.known());
    CHECK_EQ(id.key(), std::string("")); /* the settings' USB key stays empty */
    CamIdentity same = identity_for_node(root, "video0");
    CHECK(id.same_camera(same));
    CHECK_EQ(sysfs_node_key(root, "video9"), std::string("")); /* no such node */
    /* a USB camera keeps its USB key */
    uvc_camera(root, "1-9.3", "13d3", "5224", "USB FHD Webcam", 2);
    const CamIdentity usb = identity_for_node(root, "video2");
    CHECK(usb.valid());
    CHECK_EQ(usb.stable_key(), std::string("13d3:5224@1-9.3"));
    CHECK(usb.stable.empty());
    rm_tree(root);
}

void fz_m1_scan()
{
    /* the tablet: two UVC cameras on the internal hub, each with a capture node and a metadata node, no audio card */
    const std::string root = make_temp_dir("chfz");
    uvc_camera(root, "1-9.3", "13d3", "5224", "USB FHD Webcam", 0);
    uvc_camera(root, "1-9.4", "5986", "0656", "USB2.0 Camera", 2);
    FakeOptions a;
    a.profile = FakeProfile::UsbFhd;
    a.path = root + "/dev/video0";
    FakeOptions b;
    b.profile = FakeProfile::Usb2;
    b.path = root + "/dev/video2";
    b.index = 1;
    FakeCamera ca(a), cb(b);
    FakeCameraBus bus({&ca, &cb});
    const std::vector<Candidate> c = scan_cameras(bus, root + "/dev", root);
    CHECK_EQ(c.size(), 2u); /* video1 and video3 are metadata nodes (index 1): never candidates */
    if (c.size() == 2) {
        CHECK(c[0].qualifies() && c[1].qualifies());
        CHECK_EQ(c[0].ref.name, std::string("video0"));
        CHECK_EQ(c[1].ref.name, std::string("video2"));
        CHECK_EQ(c[0].ref.key(), std::string("13d3:5224@1-9.3"));
        CHECK_EQ(c[1].ref.key(), std::string("5986:0656@1-9.4"));
        CHECK_EQ(c[0].ref.card, std::string("USB FHD Webcam")); /* the fake's card is the product */
        const std::vector<CameraEntry> e = camera_entries(c);
        CHECK_EQ(e.size(), 2u);
        CHECK_EQ(e[0].label, std::string("USB FHD Webcam"));
        CHECK_EQ(e[1].label, std::string("USB2.0 Camera"));
        CHECK_EQ(pick_camera(c, ""), 0);
        CHECK_EQ(pick_camera(c, "5986:0656@1-9.4"), 1);
    }
    /* the finder lists the same, and a camera that is streaming is still listed (a second open only asks) */
    auto finder = make_system_finder(bus, root + "/dev", root);
    CHECK_EQ(finder->list().size(), 2u);
    finder->prefer("5986:0656@1-9.4");
    CameraRef r;
    CHECK(finder->find(r));
    CHECK_EQ(r.key(), std::string("5986:0656@1-9.4"));
    CHECK(!r.fallback);
    finder->prefer("0000:0000@1-1");
    CHECK(finder->find(r));
    CHECK(r.fallback); /* two present, the remembered one is not */
    CHECK_EQ(r.wanted, std::string("0000:0000@1-1"));
    CHECK_EQ(r.key(), std::string("13d3:5224@1-9.3"));
    rm_tree(root);
}

void camera_orientation()
{
    FakeOptions a;
    a.profile = FakeProfile::FineCam;
    a.orientation = 1;
    FakeCamera ca(a);
    auto bus = FakeCameraBus({&ca});
    const int fd = bus.open(FakeCamera::kPath);
    CHECK(fd >= 0);
    struct v4l2_control oc;
    std::memset(&oc, 0, sizeof oc);
    oc.id = 0x009a0922;
    CHECK_EQ(bus.ioctl(fd, VIDIOC_G_CTRL, &oc), 0);
    CHECK_EQ(oc.value, 1);
    bus.close(fd);
}

/* ------------------------------------------------------------------------------------------------ layout, settings */

void chip_layout()
{
    const int sizes[][2] = {{480, 320}, {320, 480}, {800, 480}, {480, 800}, {640, 480}, {480, 640}, {720, 720}, {640, 640}, {480, 480}};
    const int targets[] = {44, 47, 52, 63};
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
            const Rect &c = g.cam_chip;
            CHECK(c.w > 0 && c.h > 0);
            CHECK(c.h >= std::min(t, g.picture.h - 2 * g.chip_y)); /* the touch target (the picture is tall enough) */
            CHECK(c.x >= g.picture.x && c.right() <= g.picture.right());
            CHECK(c.y >= g.picture.y && c.bottom() <= g.picture.bottom());
            CHECK(c.right() == g.picture.right() - g.chip_x); /* at the top right, as the format chip is at the top left */
            CHECK(c.w <= g.picture.w * 5 / 10 + 2 * t);
        }
}

void settings_keys()
{
    Settings s;
    s.camera_id = "13d3:5224@1-9.3";
    s.camera_name = "USB FHD Webcam";
    s.sound_input = "HDA Intel PCH||";
    s.set_sizes("13d3:5224@1-9.3", Size{1280, 720}, Size{640, 360});
    s.set_sizes("5986:0656@1-9.4", Size{640, 480}, Size());
    const std::string text = settings_to_json(s, nullptr);
    Settings back;
    CHECK(settings_from_json(text, back, nullptr));
    CHECK(back == s);
    CHECK_EQ(back.camera_sizes.size(), 2u);
    const Settings::CameraSizes *c = back.sizes_of("5986:0656@1-9.4");
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->photo == (Size{640, 480}));
        CHECK(!c->video.valid());
    }
    CHECK(back.sizes_of("nope") == nullptr);
    /* a file of 0.1.1 (no new keys) reads as before: the one pair, no entries */
    Settings old;
    CHECK(settings_from_json(R"({"camera_id": "a108:2240:SN1", "photo_size": "1920x1080", "video_size": "1280x720"})", old, nullptr));
    CHECK(old.camera_sizes.empty());
    CHECK(old.photo_size == (Size{1920, 1080}));
    CHECK(old.sound_input.empty());
    CHECK(old.camera_name.empty());
    /* bad entries are ignored, the list is bounded, both invalid drops the entry */
    Settings bad;
    CHECK(settings_from_json(R"({"camera_sizes": {"a": 5, "b": {"photo": "x"}, "c": {"photo": "320x240"}}, "sound_input": 7})", bad, nullptr));
    CHECK_EQ(bad.camera_sizes.size(), 1u);
    CHECK(bad.sound_input.empty());
    Settings drop = s;
    drop.set_sizes("5986:0656@1-9.4", Size(), Size());
    CHECK_EQ(drop.camera_sizes.size(), 1u);
    Settings many;
    for (int i = 0; i < 40; ++i) many.set_sizes("k" + std::to_string(i), Size{640, 480}, Size());
    CHECK_EQ(many.camera_sizes.size(), 16u);
    CHECK(many.sizes_of("k39") != nullptr);
    CHECK(many.sizes_of("k0") == nullptr);
    /* the overrides (the scenes) */
    Settings o;
    apply_settings_overrides(o, "sound_input=none,camera_id=9:9@1-9,camera_name=Gone Cam");
    CHECK_EQ(o.sound_input, std::string("none"));
    CHECK_EQ(o.camera_id, std::string("9:9@1-9"));
    CHECK_EQ(o.camera_name, std::string("Gone Cam"));
}

/* ------------------------------------------------------------------------------------------------ the sound input */

MicCard no_cam_mic()
{
    return MicCard();
}

void sound_plans_and_rows()
{
    /* the FZ-M1: no camera microphone, one capture card (HDA Intel PCH, built in) */
    FakeAlsa tab(fake_mic_options("none,builtin"));
    const std::vector<CardInfo> cards = tab.cards();
    CHECK_EQ(cards.size(), 1u);
    CHECK_EQ(cards[0].id, std::string("PCH"));
    CHECK_EQ(cards[0].name, std::string("HDA Intel PCH"));
    CHECK(cards[0].usb_id.empty());
    SoundPlan p = plan_sound_input(tab, "", no_cam_mic());
    CHECK(p.source == SoundSource::Card && p.automatic && !p.chose_card);
    CHECK_EQ(p.label, std::string("HDA Intel PCH"));
    CHECK(p.note.empty());
    std::vector<SoundRow> rows = sound_rows(tab, "", no_cam_mic());
    CHECK_EQ(rows.size(), 3u);
    CHECK_EQ(rows[0].label, std::string("Automatic"));
    CHECK_EQ(rows[0].value, std::string("HDA Intel PCH"));
    CHECK(rows[0].chosen);
    CHECK_EQ(rows[1].label, std::string("HDA Intel PCH"));
    CHECK_EQ(rows[1].value, std::string("Built-in"));
    CHECK(!rows[1].chosen);
    CHECK_EQ(rows[2].label, std::string("No sound"));
    CHECK_EQ(rows[2].id, std::string("none"));
    /* a stored card: found by its descriptor, never by index */
    const std::string desc = sound_descriptor(cards[0]);
    CHECK_EQ(desc, std::string("HDA Intel PCH||"));
    p = plan_sound_input(tab, desc, no_cam_mic());
    CHECK(p.source == SoundSource::Card && p.chose_card && !p.automatic);
    rows = sound_rows(tab, desc, no_cam_mic());
    CHECK(!rows[0].chosen && rows[1].chosen && !rows[2].chosen);
    /* a stored card that is gone: Automatic, and the plan says so */
    p = plan_sound_input(tab, "Old Headset|/dev/snd/by-id/usb-x|0d8c:0014", no_cam_mic());
    CHECK(p.automatic && !p.chose_card && p.source == SoundSource::Card);
    CHECK_EQ(p.note, std::string("Sound input Old Headset is not here: using Automatic."));
    rows = sound_rows(tab, "Old Headset||", no_cam_mic());
    CHECK(rows[0].chosen);
    /* No sound */
    p = plan_sound_input(tab, "none", no_cam_mic());
    CHECK(p.chose_none && p.source == SoundSource::None);
    CHECK(sound_rows(tab, "none", no_cam_mic())[2].chosen);
    CHECK(open_sound(tab, p).status == OpenStatus::Failed);
    /* no card at all: nothing */
    FakeAlsa empty(fake_mic_options("none"));
    p = plan_sound_input(empty, "", no_cam_mic());
    CHECK(p.source == SoundSource::None && p.automatic);
    CHECK_EQ(sound_rows(empty, "", no_cam_mic())[0].value, std::string("No sound card"));
    /* the camera's own microphone first (Automatic), the built-in one as a row */
    FakeAlsa both(fake_mic_options("tone,builtin"));
    MicCard cam = camera_mic_by_port(both, "1-1.2");
    CHECK(cam.found);
    p = plan_sound_input(both, "", cam);
    CHECK(p.source == SoundSource::CameraMic);
    rows = sound_rows(both, "", cam);
    CHECK_EQ(rows.size(), 4u);
    CHECK_EQ(rows[0].value, std::string("Camera microphone"));
    CHECK_EQ(rows[1].label, std::string("HDA Intel PCH"));
    CHECK_EQ(rows[2].label, std::string("UGREEN camera"));
    CHECK_EQ(rows[2].value, std::string("USB"));
    p = plan_sound_input(both, sound_descriptor(both.cards()[0]), cam);
    CHECK(p.source == SoundSource::Card && p.chose_card && p.card.id == "PCH");
    /* two cards of one name: distinct labels */
    FakeMicOptions twin = fake_mic_options("none,builtin");
    FakeCard second = twin.cards[0];
    second.id = "PCH2";
    second.usb_port = "1-2";
    second.usb_id = "0d8c:0014";
    twin.cards.push_back(second);
    FakeAlsa tw(twin);
    rows = sound_rows(tw, "", no_cam_mic());
    CHECK(rows.size() == 4 && rows[1].label != rows[2].label);
}

OpenResult open_with(const char *spec, std::string setting = "")
{
    FakeAlsa fa(fake_mic_options(spec));
    const SoundPlan p = plan_sound_input(fa, setting, no_cam_mic());
    return open_sound(fa, p);
}

void sound_open_policy()
{
    /* the built-in card: 48 kHz stereo 16-bit through hw: (the card's own rate first, Cassette's policy) */
    OpenResult r = open_with("none,builtin");
    CHECK(r.ok());
    CHECK_EQ(r.format.rate, 48000u);
    CHECK_EQ(static_cast<int>(r.format.channels), 2);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    CHECK(!r.via_server);
    CHECK_EQ(r.device, std::string("hw:CARD=PCH,DEV=0"));
    /* mono: kept mono (the recorder takes any channel count) */
    r = open_with("none,builtin-mono");
    CHECK(r.ok());
    CHECK_EQ(static_cast<int>(r.format.channels), 1);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    /* 32-bit only: read as 32-bit, cut to 16 */
    r = open_with("none,builtin-s32");
    CHECK(r.ok());
    CHECK(r.conv == Conversion::S32_to_S16);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    /* held by a sound server: the server's default device */
    r = open_with("none,builtin-busy");
    CHECK(r.ok());
    CHECK(r.via_server);
    CHECK_EQ(r.device, std::string("default"));
    CHECK_EQ(r.format.rate, 48000u);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    /* hw: refuses what it lists: plughw: at 48 kHz stereo */
    r = open_with("none,builtin-plug");
    CHECK(r.ok());
    CHECK_EQ(r.device, std::string("plughw:CARD=PCH,DEV=0"));
    CHECK_EQ(r.format.rate, 48000u);
    CHECK_EQ(static_cast<int>(r.format.channels), 2);
    CHECK_EQ(static_cast<int>(r.format.bits), 16);
    /* dead: the error is the card's */
    r = open_with("none,builtin-dead");
    CHECK(!r.ok());
    CHECK(r.message.find("Input/output error") != std::string::npos);
    {
        FakeAlsa fa(fake_mic_options("none,builtin-dead"));
        const SoundPlan p = plan_sound_input(fa, "", no_cam_mic());
        const OpenResult o = open_sound(fa, p);
        CHECK_EQ(sound_failure_text(p, o), std::string("Sound input HDA Intel PCH failed: Cannot open the card: Input/output error."));
    }
    /* the camera's own card: hw: only, as 0.1.1 (busy is refused, no server route) */
    {
        FakeAlsa fa(fake_mic_options("busy,builtin"));
        const MicCard cam = camera_mic_by_port(fa, "1-1.2");
        const SoundPlan p = plan_sound_input(fa, "", cam);
        CHECK(p.source == SoundSource::CameraMic);
        const OpenResult o = open_sound(fa, p);
        CHECK(!o.ok());
        CHECK(o.status == OpenStatus::Busy);
        CHECK(fa.opened().empty());
    }
}

struct PeakSink : FrameSink {
    std::atomic<std::uint64_t> frames{0};
    std::size_t write(const void *f, std::size_t n) override
    {
        (void)f;
        frames += n;
        return n;
    }
};

void sound_conversions()
{
    const std::int32_t s32[3] = {0x12345678, static_cast<std::int32_t>(0x80000000u), -1};
    std::int16_t out[3];
    convert_s32_to_s16(s32, out, 3);
    CHECK_EQ(static_cast<int>(out[0]), 0x1234);
    CHECK_EQ(static_cast<int>(out[1]), -32768);
    CHECK_EQ(static_cast<int>(out[2]), -1);
    const std::uint8_t s24[6] = {0x01, 0x56, 0x34, 0xFF, 0xFF, 0xFF}; /* 0x345601, -1 */
    convert_s24_3_to_s16(s24, out, 2);
    CHECK_EQ(static_cast<int>(out[0]), 0x3456);
    CHECK_EQ(static_cast<int>(out[1]), -1);
    /* choose_params: want_bits 16 on a card without S16 */
    CaptureOffer o;
    o.rates = {44100};
    o.min_channels = o.max_channels = 2;
    o.formats = {SampleFormat::S24_3LE};
    HwRequest req;
    Conversion conv;
    AudioFormat fmt;
    OpenOptions oo;
    oo.want_bits = 16;
    CHECK(choose_params(o, oo, req, conv, fmt, nullptr));
    CHECK(conv == Conversion::S24_3_to_S16);
    CHECK_EQ(static_cast<int>(fmt.bits), 16);
    CHECK_EQ(req.rate, 44100u);
    oo.want_bits = 0; /* Cassette's own order is unchanged: 24-bit stays 24-bit */
    CHECK(choose_params(o, oo, req, conv, fmt, nullptr));
    CHECK(conv == Conversion::None);
    CHECK_EQ(static_cast<int>(fmt.bits), 24);
    /* a capture from a 32-bit-only card delivers 16-bit samples (the fake's tone is -20 dBFS, 3277) */
    FakeAlsa fa(fake_mic_options("none,builtin-s32"));
    const SoundPlan p = plan_sound_input(fa, "", no_cam_mic());
    OpenResult r = open_sound(fa, p);
    CHECK(r.ok());
    PeakSink sink;
    std::atomic<int> peak{0};
    CaptureOptions co;
    co.on_period = [&peak](const void *f, size_t n, const AudioFormat &fm) {
        const auto *v = static_cast<const std::int16_t *>(f);
        for (size_t i = 0; i < n * fm.channels; ++i) peak = std::max(peak.load(), std::abs(static_cast<int>(v[i])));
    };
    AudioCapture cap(std::move(r), sink, co);
    CHECK(cap.start());
    CHECK(wait_for(2000, [&] { return sink.frames.load() >= 4096; }));
    cap.stop();
    CHECK(peak.load() > 3000 && peak.load() < 3600);
}

/* ------------------------------------------------------------------------------------------------ the controller */

ControllerConfig two_cameras(const std::string &folder, const std::string &mic = "none,builtin", const std::string &set = "")
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.fake_opt.profile = FakeProfile::UsbFhd;
    FakeOptions b = c.fake_opt;
    b.profile = FakeProfile::Usb2;
    b.path = std::string(FakeCamera::kPath) + "1";
    b.index = 1;
    c.fake_more.push_back(b);
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = "bcm2837";
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

const char *kFhd = "13d3:5224@1-9.3";
const char *kUsb2 = "5986:0656@1-9.4";

void controller_two_cameras()
{
    const std::string dir = make_temp_dir("chctl");
    Controller ctl(two_cameras(dir));
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.cameras == 2; }, &s));
    CHECK_EQ(s.camera, std::string("USB FHD Webcam"));
    CHECK_EQ(s.camera_key, std::string(kFhd));
    CHECK_EQ(s.camera_label, std::string("USB FHD Webcam"));
    CHECK_EQ(ctl.settings().camera_id, std::string(kFhd)); /* remembered as it opened */
    CHECK_EQ(ctl.settings().camera_name, std::string("USB FHD Webcam"));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1920x1080:30"));
    CHECK(s.notice.text.empty());

    /* the quick switch: the next camera, the old one closed and the new one opened; remembered by id */
    const std::uint64_t gen0 = s.gen;
    CHECK(ctl.next_camera());
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kUsb2 && x.mode.valid(); }, &s));
    CHECK(s.gen > gen0);
    CHECK_EQ(s.camera, std::string("USB2.0 Camera"));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:640x480:30")); /* its best mode */
    CHECK_EQ(ctl.settings().camera_id, std::string(kUsb2));
    CHECK_EQ(ctl.settings().camera_name, std::string("USB2.0 Camera"));
    CHECK_EQ(s.cameras, 2);
    CHECK(s.notice.text.empty()); /* a switch says nothing: the chip shows it */
    /* and back */
    CHECK(ctl.next_camera());
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kFhd && x.mode.valid(); }, &s));
    CHECK_EQ(mode_arg(s.mode), std::string("MJPG:1920x1080:30"));

    /* the sizes are per camera: 1280x720 for the first one does not touch the second; each returns as it was left */
    {
        Settings st = ctl.settings();
        st.photo_size = Size{1280, 720};
        ctl.settings_apply(st);
        CHECK(ctl.settings().sizes_of(kFhd) != nullptr);
        ctl.stop(); /* the Settings page releases the camera; leaving it reopens it with the new values */
        ctl.start();
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.mode.w == 1280; }, &s));
        CHECK(ctl.next_camera());
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kUsb2 && x.mode.valid(); }, &s));
        CHECK_EQ(ctl.selection().photo.w, 640); /* the best mode: no size of the first camera is carried over */
        CHECK(!ctl.settings().photo_size.valid());
        CHECK(ctl.next_camera());
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kFhd && x.mode.valid(); }, &s));
        CHECK_EQ(s.mode.w, 1280);
        CHECK(ctl.settings().photo_size == (Size{1280, 720}));
    }

    /* Settings > Camera: the list, a choice remembered while the camera is released, opened when it comes back */
    ctl.stop();
    ctl.refresh_cameras();
    CHECK_EQ(ctl.cameras().size(), 2u);
    CHECK(ctl.select_camera(kUsb2));
    CHECK_EQ(ctl.settings().camera_id, std::string(kUsb2));
    CHECK(!ctl.select_camera("0000:0000@1-1")); /* not listed */
    ctl.start();
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.camera_key == kUsb2 && x.mode.valid(); }, &s));

    /* the self-timer: no switch while it counts (a toast says why) */
    ctl.stop();
    rm_tree(dir);
}

void controller_switch_refused()
{
    const std::string dir = make_temp_dir("chref");
    Controller ctl(two_cameras(dir, "none,builtin", "self_timer_s=10"));
    ctl.start();
    Snapshot s;
    CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.cameras == 2; }, &s));
    ctl.shutter(mono_ms());
    CHECK(ctl.counting_down());
    CHECK(!ctl.next_camera());
    s = ctl.step(mono_ms());
    CHECK_EQ(s.notice.text, std::string("Cancel the self-timer to change the camera."));
    CHECK_EQ(s.camera_key, std::string(kFhd)); /* nothing switched */
    CHECK(ctl.cancel_countdown());
    ctl.stop();
    rm_tree(dir);
}

void controller_remembered()
{
    const std::string dir = make_temp_dir("chrem");
    /* two cameras, the remembered one is not plugged in: the first is used, the toast says so once, nothing is overwritten */
    {
        const std::string file = dir + "/settings.json";
        write_file(file, R"({"camera_id": "9999:0001@1-9.9", "camera_name": "Gone Cam"})");
        ControllerConfig c = two_cameras(dir + "/f");
        c.settings_path = file;
        Controller ctl(c);
        ctl.start();
        Snapshot s;
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.notice.text.find("not plugged in") != std::string::npos; }, &s));
        CHECK_EQ(s.notice.text, std::string("Gone Cam is not plugged in: using USB FHD Webcam."));
        CHECK_EQ(ctl.settings().camera_id, std::string("9999:0001@1-9.9")); /* still the remembered one */
        CHECK_EQ(s.camera_key, std::string(kFhd));
        /* choosing the camera in use makes it the remembered one */
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.cameras == 2; }));
        CHECK(ctl.select_camera(kFhd));
        CHECK_EQ(ctl.settings().camera_id, std::string(kFhd));
        CHECK_EQ(ctl.settings().camera_name, std::string("USB FHD Webcam"));
        ctl.stop();
    }
    /* one camera, a different one remembered: used silently and remembered, as 0.1.1 */
    {
        const std::string file = dir + "/single.json";
        write_file(file, R"({"camera_id": "9999:0001@1-9.9", "camera_name": "Gone Cam"})");
        ControllerConfig c;
        c.fake = true;
        c.fake_opt.frames = 2;
        c.persist = false;
        c.log_every = 1000;
        c.ui_tid = current_tid();
        c.settings_path = file;
        Controller ctl(c);
        ctl.start();
        Snapshot s;
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming; }, &s));
        CHECK(step_until(ctl, 500, [&](const Snapshot &x) { return x.camera_key == "a108:2240:FAKE0001"; }));
        CHECK(ctl.step(mono_ms()).notice.text.empty());
        CHECK_EQ(ctl.settings().camera_id, std::string("a108:2240:FAKE0001"));
        CHECK(ctl.step(mono_ms()).cameras < 2);
        /* 'next camera' with one camera does nothing */
        CHECK(!ctl.next_camera());
        ctl.stop();
    }
    rm_tree(dir);
}

/* a scene with sound on the controller (needs FFmpeg and the fake camera's MJPEG) */
bool avail(const AvLibs *&av)
{
    std::string why;
    av = real_av_libs(&why);
    if (!av) {
        SKIP(("choosers recording: no FFmpeg libraries (" + why + ")").c_str());
        return false;
    }
    if (!TurboJpeg::get().can_encode()) {
        SKIP("choosers recording: no libturbojpeg.so.0 (the fake camera's MJPEG)");
        return false;
    }
    return true;
}

ControllerConfig one_camera(const std::string &folder, const std::string &mic, const std::string &set = "")
{
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = folder;
    c.soc_override = "bcm2837";
    c.clock_synced = 0;
    c.utc = true;
    c.fake_mic = mic;
    c.settings_overrides = set;
    return c;
}

bool streaming(Controller &ctl)
{
    if (!step_until(ctl, 3000, [](const Snapshot &x) { return x.state == CamState::Streaming; })) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    return true;
}

/* REC, wait for the first frame, record about a second, stop: the snapshot while recording */
Snapshot record_a_bit(Controller &ctl)
{
    Snapshot s;
    ctl.record(mono_ms());
    step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started; }, &s);
    step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started && x.rec_ms >= 800; }, &s);
    const Snapshot during = s;
    ctl.record(mono_ms());
    step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; });
    return during;
}

void controller_sound(const std::string &dir)
{
    const AvLibs *av = nullptr;
    if (!avail(av)) return;
    /* the tablet: no camera microphone, the built-in card: Automatic records with sound from it */
    for (const char *mic : {"none,builtin", "none,builtin-mono", "none,builtin-s32", "none,builtin-busy", "none,builtin-plug"}) {
        Controller ctl(one_camera(dir + "/a-" + mic, mic));
        ctl.start();
        CHECK(streaming(ctl));
        const Snapshot during = record_a_bit(ctl);
        CHECK(during.recording && during.rec_audio);
        CHECK(during.rec_sound_other);
        CHECK(ctl.last_recording().audio);
        CHECK(ctl.last_recording().frames > 10);
        CHECK(ctl.last_recording().stop == RecStop::User);
        CHECK(ctl.step(mono_ms()).notice.text.empty() || ctl.step(mono_ms()).notice.text != kNoticeNoMic);
        ctl.stop();
    }
    /* the camera's own microphone first; the built-in card chosen in Settings instead; No sound */
    {
        Controller ctl(one_camera(dir + "/b", "tone,builtin"));
        ctl.start();
        CHECK(streaming(ctl));
        Snapshot during = record_a_bit(ctl);
        CHECK(during.rec_audio && !during.rec_sound_other); /* Automatic: the camera's */
        Settings st = ctl.settings();
        FakeAlsa names(fake_mic_options("tone,builtin"));
        st.sound_input = sound_descriptor(names.cards()[0]); /* the PCH */
        ctl.settings_apply(st);
        during = record_a_bit(ctl);
        CHECK(during.rec_audio && during.rec_sound_other); /* the PCH */
        st.sound_input = "none";
        ctl.settings_apply(st);
        during = record_a_bit(ctl);
        CHECK(during.recording && !during.rec_audio);
        CHECK(!ctl.last_recording().audio);
        CHECK(ctl.step(mono_ms()).notice.text.empty()); /* chosen: no toast */
        ctl.stop();
    }
    /* a stored card that cannot be opened: shown, nothing recorded; Automatic on the same card: video only + toast */
    {
        FakeAlsa fa(fake_mic_options("none,builtin-dead"));
        const std::string desc = sound_descriptor(fa.cards()[0]);
        Controller ctl(one_camera(dir + "/c", "none,builtin-dead", "sound_input=" + desc));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        CHECK(!ctl.recording());
        Snapshot s = ctl.step(mono_ms());
        CHECK_EQ(s.notice.text, std::string("Sound input HDA Intel PCH failed: Cannot open the card: Input/output error."));
        ctl.stop();
        Controller auto_ctl(one_camera(dir + "/d", "none,builtin-dead"));
        auto_ctl.start();
        CHECK(streaming(auto_ctl));
        const Snapshot during = record_a_bit(auto_ctl);
        CHECK(during.recording && !during.rec_audio);
        CHECK(!auto_ctl.last_recording().audio);
        auto_ctl.stop();
    }
    /* no sound card at all: video only with 0.1.1's toast */
    {
        Controller ctl(one_camera(dir + "/e", "none"));
        ctl.start();
        CHECK(streaming(ctl));
        ctl.record(mono_ms());
        CHECK(ctl.recording());
        CHECK(step_until(ctl, 2000, [](const Snapshot &x) { return x.notice.text == kNoticeNoMic; }));
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }));
        ctl.stop();
    }
    /* no camera switch while recording */
    {
        Controller ctl(two_cameras(dir + "/f", "none,builtin"));
        ctl.start();
        Snapshot s;
        CHECK(step_until(ctl, 4000, [](const Snapshot &x) { return x.state == CamState::Streaming && x.cameras == 2; }, &s));
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return x.rec_started; }));
        CHECK(!ctl.next_camera());
        CHECK(!ctl.select_camera(kUsb2));
        s = ctl.step(mono_ms());
        CHECK_EQ(s.notice.text, std::string("Stop the recording to change the camera."));
        CHECK_EQ(s.camera_key, std::string(kFhd));
        CHECK(ctl.recording());
        ctl.record(mono_ms());
        CHECK(step_until(ctl, 3000, [](const Snapshot &x) { return !x.recording; }));
        ctl.stop();
    }
}

} // namespace

void test_choosers()
{
    entries_and_labels();
    non_usb_identity();
    fz_m1_scan();
    camera_orientation();
    chip_layout();
    settings_keys();
    sound_plans_and_rows();
    sound_open_policy();
    sound_conversions();
    controller_two_cameras();
    controller_switch_refused();
    controller_remembered();
    const std::string dir = make_temp_dir("chsnd");
    controller_sound(dir);
    rm_tree(dir);
}
