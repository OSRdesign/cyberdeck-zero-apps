/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the settings file (section 11: defaults, tolerant reads, clamps, the prefix rule, round trip, unknown keys
 * kept, written only on a change), the toast line (timed notices one after the other, the standing notice), the
 * under-voltage rate limit (once a minute), the codec nodes by name, MemTotal.
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "app/notice_queue.hpp"
#include "modes/codec_caps.hpp"
#include "power/undervolt.hpp"
#include "scenes/fileops.hpp"
#include "settings/settings.hpp"

#include <fstream>
#include <iterator>

using namespace vhs;

namespace {

std::string read_all(const std::string &p)
{
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void defaults_and_clamps()
{
    Settings s;
    CHECK_EQ(s.folder, std::string("~/VHS"));
    CHECK_EQ(s.prefix, std::string("scene"));
    CHECK(!s.have_photo_mode);
    CHECK(!s.photo_size.valid());
    CHECK_EQ(s.self_timer_s, 0);
    CHECK(s.split);
    CHECK_EQ(s.split_mib, 4096);
    CHECK_EQ(s.min_free_mb, 500);
    Settings r;
    CHECK(settings_from_json(R"({"prefix":"Stopwatch test","photo_mode":"light","photo_size":"1280x960",
        "video_size":"1280x720","self_timer_s":4,"split":false,"output":"usb","split_mib":10,"min_free_mb":9999999,
        "camera_id":"a108:2240:SN1","folder":"/media/x"})",
                             r, nullptr));
    CHECK_EQ(r.prefix, std::string("Stopwatch test"));
    CHECK(r.have_photo_mode && r.photo_mode == PhotoMode::Light);
    CHECK(r.photo_size == (Size{1280, 960}));
    CHECK(r.video_size == (Size{1280, 720}));
    CHECK_EQ(r.self_timer_s, 3); /* 4 moves to the nearest: 3 (a tie goes to the shorter) */
    CHECK(!r.split);
    CHECK_EQ(r.output, std::string("usb"));
    CHECK_EQ(r.split_mib, 64);       /* clamped */
    CHECK_EQ(r.min_free_mb, 100000); /* clamped */
    CHECK_EQ(r.camera_id, std::string("a108:2240:SN1"));
    CHECK_EQ(r.folder, std::string("/media/x"));
    /* bad values: that key's default */
    CHECK(settings_from_json(R"({"prefix":" bad","photo_mode":"quick","photo_size":"big","self_timer_s":"x",
        "split":"yes"})",
                             r, nullptr));
    CHECK_EQ(r.prefix, std::string("scene"));
    CHECK(!r.have_photo_mode);
    CHECK(!r.photo_size.valid());
    CHECK_EQ(r.self_timer_s, 0);
    CHECK(r.split);
    CHECK(!settings_from_json("not json", r, nullptr));
    CHECK(!settings_from_json("[1,2]", r, nullptr));
    CHECK(r == Settings());
    CHECK_EQ(clamp_self_timer(7), 5);
    CHECK_EQ(clamp_self_timer(8), 10);
    CHECK_EQ(clamp_self_timer(99), 10);
    CHECK_EQ(clamp_self_timer(-1), 0);
    CHECK(valid_prefix("scene"));
    CHECK(valid_prefix("a_b-c 9"));
    CHECK(!valid_prefix(""));
    CHECK(!valid_prefix(".hidden"));
    CHECK(!valid_prefix("trailing "));
    CHECK(!valid_prefix("slash/no"));
    CHECK(!valid_prefix("x234567890123456789012345")); /* 25 */
}

void store()
{
    const std::string dir = make_temp_dir("settings");
    const std::string path = dir + "/sub/settings.json";
    SettingsStore st(posix_file_ops());
    st.load(path); /* missing: the defaults */
    CHECK(st.get() == Settings());
    Settings s = st.get();
    CHECK_EQ(st.set(s), 1); /* unchanged: not written */
    CHECK_EQ(st.writes(), 0);
    s.camera_id = "a108:2240:SN1";
    s.photo_size = Size{1920, 1080};
    CHECK_EQ(st.set(s), 0);
    CHECK_EQ(st.writes(), 1);
    const std::string text = read_all(path);
    CHECK(text.find("\"camera_id\": \"a108:2240:SN1\"") != std::string::npos);
    CHECK(text.find("\"photo_size\": \"1920x1080\"") != std::string::npos);
    CHECK(text.find("photo_mode") == std::string::npos); /* absent: the SoC default */
    SettingsStore again(posix_file_ops());
    again.load(path);
    CHECK(again.get() == s);
    /* unknown keys survive a write */
    write_file(path, R"({"future_key": 42, "prefix": "take"})");
    SettingsStore third(posix_file_ops());
    third.load(path);
    CHECK_EQ(third.get().prefix, std::string("take"));
    Settings t = third.get();
    t.camera_id = "x:y:z";
    CHECK_EQ(third.set(t), 0);
    CHECK(read_all(path).find("future_key") != std::string::npos);
    /* read-only (the fake camera): never written */
    SettingsStore ro(posix_file_ops());
    ro.load(dir + "/ro.json");
    ro.set_read_only(true);
    Settings u = ro.get();
    u.camera_id = "q";
    CHECK_EQ(ro.set(u), 0);
    CHECK_EQ(ro.get().camera_id, std::string("q"));
    CHECK(read_all(dir + "/ro.json").empty());
    rm_tree(dir);
}

void notices()
{
    NoticeQueue q(4000);
    CHECK(q.current(0).text.empty());
    q.push(Notice{kNoticeUnplugged, false});
    q.push(Notice{kNoticePower, false});
    q.push(Notice{kNoticeUnplugged, false}); /* waiting already: not twice */
    CHECK_EQ(q.waiting(), static_cast<std::size_t>(2));
    CHECK_EQ(q.current(100).text, std::string(kNoticeUnplugged));
    q.push(Notice{kNoticeUnplugged, false}); /* shown: not again */
    CHECK_EQ(q.current(4000).text, std::string(kNoticeUnplugged));
    CHECK_EQ(q.current(4100).text, std::string(kNoticePower)); /* 4 s each, then the next */
    CHECK(q.current(8200).text.empty());
    /* the standing notice shows while no timed one does */
    q.set_standing(Notice{kNoticeBusy, false});
    CHECK_EQ(q.current(9000).text, std::string(kNoticeBusy));
    q.push(Notice{kNoticePower, false});
    CHECK_EQ(q.current(9100).text, std::string(kNoticePower));
    CHECK_EQ(q.current(13200).text, std::string(kNoticeBusy));
    q.dismiss(13300); /* hidden until it changes */
    CHECK(q.current(13400).text.empty());
    q.set_standing(Notice{kNoticeNoAccess, false});
    CHECK_EQ(q.current(13500).text, std::string(kNoticeNoAccess));
    q.set_standing(Notice());
    CHECK(q.current(13600).text.empty());
    /* a dismissed timed notice makes room for the next at once */
    q.push(Notice{kNoticeDiskFull, true});
    q.push(Notice{kNoticeNoSound, false});
    const Notice n = q.current(20000);
    CHECK(n.red);
    q.dismiss(20100);
    CHECK_EQ(q.current(20200).text, std::string(kNoticeNoSound));
    q.push(Notice{"", false});
    CHECK_EQ(q.waiting(), static_cast<std::size_t>(0));
}

void undervolt()
{
    UndervoltNotice u(60000);
    CHECK(!u.step(false, 0));
    CHECK(u.step(true, 1000));
    CHECK(!u.step(true, 2000));
    CHECK(!u.step(true, 60999));
    CHECK(u.step(true, 61000)); /* at most once every 60 s */
    CHECK(!u.step(false, 200000));
    CHECK(u.step(true, 200001));
}

void codecs_and_ram()
{
    const std::string root = make_temp_dir("codec");
    const std::string d = root + "/sys/class/video4linux";
    make_dirs(d + "/video10");
    make_dirs(d + "/video11");
    make_dirs(d + "/video19");
    make_dirs(d + "/video0");
    write_file(d + "/video10/name", "bcm2835-codec-decode\n");
    write_file(d + "/video11/name", "bcm2835-codec-encode\n");
    write_file(d + "/video19/name", "rpi-hevc-dec\n");
    write_file(d + "/video0/name", "UGREEN FineCam 1080P CM969: U\n");
    CodecCaps c = detect_codec_caps(root, "/dev");
    CHECK_EQ(c.h264_encode, std::string("/dev/video11"));
    CHECK_EQ(c.h264_decode, std::string("/dev/video10"));
    CHECK_EQ(c.hevc_decode, std::string("/dev/video19"));
    CHECK(codec_caps_text(c).find("h264 encode /dev/video11") != std::string::npos);
    const CodecCaps none = detect_codec_caps(root + "/nothing", "/dev");
    CHECK_EQ(codec_caps_text(none), std::string("codecs: h264 encode none, h264 decode none, hevc decode none"));
    make_dirs(root + "/proc");
    write_file(root + "/proc/meminfo", "MemTotal:         427324 kB\nMemFree:  1 kB\n");
    CHECK_EQ(ram_mb(root), 417);
    CHECK_EQ(ram_mb(root + "/nothing"), -1);
    rm_tree(root);
}

} // namespace

void test_settings()
{
    defaults_and_clamps();
    store();
    notices();
    undervolt();
    codecs_and_ram();
}
