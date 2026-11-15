/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: playback's video side (brief 15.1 "Playback": presenter picks by clock; the drop rule and its 2 s notice;
 * MJPEG exact seek; no-audio clock), the own YUV-to-RGB with fit (brief 4.4), the Playback page geometry (canvas
 * PlaybackWindowed-480x320), the H.264 path through FFmpeg's decoder (and its thumbnail), and the playback numbers of the
 * build box (a 10 s 720p fixture through the movie thread and the presenter at real time: CPU and drops; NOT board
 * numbers).
 */
#include "check.hpp"

#include "app/controller.hpp"
#include "jpeg/turbojpeg.hpp"
#include "layout/playback_layout.hpp"
#include "live/clock.hpp"
#include "live/convert.hpp"
#include "movie/movie.hpp"
#include "movie/presenter.hpp"
#include "movie/video_decoder.hpp"
#include "record/av_libs.hpp"
#include "record/recorder.hpp"
#include "scenes/fileops.hpp"

#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

using namespace vhs;

namespace {

std::shared_ptr<Picture> pic(int tag)
{
    auto p = std::make_shared<Picture>();
    p->w = tag;
    return p;
}

void presenter()
{
    Presenter pr(3);
    CHECK(!pr.full());
    pr.push({pic(1), 0});
    pr.push({pic(2), 33});
    pr.push({pic(3), 67});
    CHECK(pr.full());
    std::int64_t pts = -2;
    CHECK(pr.pick(-1, &pts) == nullptr); /* nothing due yet */
    CHECK_EQ(pts, -1ll);
    auto a = pr.pick(10, &pts);
    CHECK(a && a->w == 1 && pts == 0);
    auto b = pr.pick(70, &pts); /* two due: the newest, the older one dropped */
    CHECK(b && b->w == 3 && pts == 67);
    CHECK_EQ(pr.dropped(), 1ull);
    CHECK_EQ(pr.size(), static_cast<std::size_t>(0));
    pr.push({pic(4), 100});
    CHECK_EQ(pr.next_pts(), 100ll);
    pr.clear();
    CHECK(pr.pick(1000) == nullptr);

    /* the no-audio clock: runs while playing, stops on pause, set by a seek */
    PlayClock c;
    c.set(0, 1000);
    CHECK_EQ(c.at(1500), 0ll); /* not running yet */
    c.resume(1000);
    CHECK_EQ(c.at(1500), 500ll);
    c.pause(2000);
    CHECK_EQ(c.at(5000), 1000ll);
    c.set(7000, 5000);
    CHECK_EQ(c.at(6000), 7000ll); /* paused: a seek moves it, it stays */
    c.resume(6000);
    CHECK_EQ(c.at(6250), 7250ll);

    /* the drop notice: drops that keep coming for 2 s, once */
    DropWindow w;
    CHECK(!w.drop(0));
    CHECK(!w.drop(500));
    CHECK(!w.drop(1700)); /* a gap over 600 ms starts again */
    CHECK(!w.drop(2200));
    CHECK(!w.drop(2700));
    CHECK(!w.drop(3200));
    CHECK(w.drop(3700)); /* 1700 .. 3700: 2 s of drops */
    CHECK(!w.drop(3800)); /* once */
    CHECK(w.fired());
}

void yuv()
{
    /* a 4x2 YUV420P picture: Y 16 (black) / 235 (white) / 81,90,240 (red); fit 1:1 and 2:1 */
    const int w = 4, h = 2;
    std::uint8_t Y[8] = {16, 235, 145, 145, 16, 235, 145, 145}; /* black, white (neutral chroma), green (U 54, V 34) */
    std::uint8_t U[2] = {128, 54};
    std::uint8_t V[2] = {128, 34};
    YuvPlanes p;
    p.y = Y;
    p.u = U;
    p.v = V;
    p.ys = 4;
    p.us = 2;
    p.vs = 2;
    p.vshift = 1;
    std::uint8_t out[4 * 2 * 4];
    yuv_fit(PixFmt::XRGB8888, p, w, h, out, 4, 2, 16);
    CHECK(out[0] < 4 && out[1] < 4 && out[2] < 4);              /* black (B, G, R) */
    CHECK(out[4] > 250 && out[5] > 250 && out[6] > 250);        /* white */
    CHECK(out[8 + 1] > 150 && out[8 + 2] < 60);                 /* Y 145, U 54, V 34: green */
    CHECK(out[16 + 4] > 250 && out[16] < 4);                    /* the second row shares the chroma (4:2:0) */
    /* full range (YUVJ): 0 is black, 255 white */
    std::uint8_t Yf[4] = {0, 255, 0, 255};
    std::uint8_t Uf[1] = {128}, Vf[1] = {128};
    YuvPlanes f;
    f.y = Yf;
    f.u = Uf;
    f.v = Vf;
    f.ys = 2;
    f.us = 1;
    f.vs = 1;
    f.full_range = true;
    std::uint8_t o2[2 * 2 * 4];
    yuv_fit(PixFmt::XRGB8888, f, 2, 2, o2, 2, 2, 8);
    CHECK(o2[0] == 0 && o2[4] == 255);
    /* NV12 and the fit to a smaller box (nearest sample) */
    std::uint8_t Yn[16];
    for (int i = 0; i < 16; ++i) Yn[i] = static_cast<std::uint8_t>(i < 8 ? 16 : 235);
    std::uint8_t UV[8] = {128, 128, 128, 128, 128, 128, 128, 128};
    YuvPlanes n;
    n.y = Yn;
    n.u = UV;
    n.ys = 4;
    n.us = 4;
    n.nv12 = true;
    std::uint8_t o3[2 * 2 * 2];
    yuv_fit(PixFmt::RGB565, n, 4, 4, o3, 2, 2, 4);
    const std::uint16_t top = static_cast<std::uint16_t>(o3[0] | (o3[1] << 8)), bottom = static_cast<std::uint16_t>(o3[4] | (o3[5] << 8));
    CHECK(top == 0x0000);
    CHECK(bottom == 0xFFFF);
}

void layout()
{
    CaptureLayoutIn in;
    in.w = 480;
    in.h = 320;
    in.body_y = 40;
    in.target = 44;
    const PlaybackLayout l = playback_layout(in, 42);
    CHECK(l.column);
    CHECK(l.picture.x == 16 && l.picture.y == 52 && l.picture.w == 352 && l.picture.h == 198);
    CHECK(l.pause.x == 376 && l.pause.y == 52 && l.pause.w == 88 && l.pause.h == 56); /* canvas (368, 8) in the panel */
    CHECK(l.minus.x == 376 && l.minus.y == 116 && l.minus.w == 42);
    CHECK(l.plus.x == 422 && l.plus.w == 42);
    CHECK(l.minus.h >= 44);                                                          /* at least TARGET */
    CHECK(l.bar.x == 20 && l.bar.y == 266 && l.bar.w == 344);                          /* canvas (12, 222), 344 */
    CHECK(l.time_left.y == 278 && l.name.y == 290);
    CHECK(l.bar_hit.h >= 42);
    CHECK(l.name.y + l.name.h <= l.panel.y + l.panel.h);
    CHECK_EQ(seek_ms_at(l, 20, 10000), 0ll);
    CHECK_EQ(seek_ms_at(l, 20 + 172, 10000), 5000ll);
    CHECK_EQ(seek_ms_at(l, 999, 10000), 10000ll);
    CHECK_EQ(seek_ms_at(l, 0, 10000), 0ll);
    CHECK_EQ(seek_x_of(l, 5000, 10000), 192);
    CHECK_EQ(seek_x_of(l, 5000, 0), 20);
    in.w = 320;
    in.h = 480;
    const PlaybackLayout p = playback_layout(in, 42);
    CHECK(!p.column);
    CHECK(p.pause.y > p.picture.y + p.picture.h && p.pause.h >= 44 && p.minus.h >= 44);
    CHECK(p.plus.x + p.plus.w <= p.panel.x + p.panel.w);
    CHECK(p.name.y + p.name.h <= p.panel.y + p.panel.h);
}

/* the testsrc fixture's JPEG frames */
std::vector<std::string> jpegs()
{
    std::vector<std::string> out;
    const std::string p = fixture("testsrc-720.mjpg");
    if (p.empty()) return out;
    std::ifstream in(p, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    while (true) {
        const std::size_t s = all.find("\xFF\xD8", at);
        if (s == std::string::npos) break;
        const std::size_t e = all.find("\xFF\xD9", s + 2);
        if (e == std::string::npos) break;
        out.push_back(all.substr(s, e + 2 - s));
        at = e + 2;
    }
    return out;
}

bool make_mjpeg_mkv(const AvLibs &av, const std::vector<std::string> &js, const std::string &folder, const std::string &stem, int n)
{
    RecorderConfig rc;
    rc.av = &av;
    rc.ops = &posix_file_ops();
    rc.folder = folder;
    rc.stem = stem;
    rc.video.w = 1280;
    rc.video.h = 720;
    rc.ring_seconds = 60;
    Recorder r(rc);
    r.start();
    for (int i = 0; i < n; ++i) {
        const std::string &f = js[static_cast<std::size_t>(i) % js.size()];
        r.push_video(reinterpret_cast<const std::uint8_t *>(f.data()), f.size(), 9000000 + i * 33333LL);
    }
    wait_for(10000, [&] { return r.status().frames == static_cast<std::uint64_t>(n); });
    r.request_stop(RecStop::User);
    r.join();
    return r.status().frames == static_cast<std::uint64_t>(n);
}

double cpu_s()
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

/* plays a file through the movie thread and a 30 Hz presenter loop for `secs` at real time: shown, dropped, core share */
void bench(const AvLibs &av, const std::string &file, const char *what, double secs, int box_w, int box_h)
{
    PlayClock clock;
    std::mutex m;
    const std::int64_t t0 = mono_ms();
    {
        std::lock_guard<std::mutex> lock(m);
        clock.set(0, t0);
        clock.resume(t0);
    }
    Movie mv(av, [&] {
        std::lock_guard<std::mutex> lock(m);
        return clock.at(mono_ms());
    }, [] { return true; });
    std::string why;
    CHECK(mv.open(file, box_w, box_h, PixFmt::RGB565, &why));
    const double c0 = cpu_s();
    int shown = 0;
    while (mono_ms() - t0 < static_cast<std::int64_t>(secs * 1000)) {
        std::int64_t now;
        {
            std::lock_guard<std::mutex> lock(m);
            now = clock.at(mono_ms());
        }
        if (mv.presenter().pick(now)) ++shown;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    const double cpu = cpu_s() - c0;
    const MovieStatus st = mv.status();
    mv.close();
    std::fprintf(stderr, "playback bench (build box, NOT a board): %s %dx%d -> %dx%d box: %d shown in %.1f s (%.1f fps), %llu decoded "
                         "(%.1f ms each), %llu dropped late, %llu not shown, %.2f core\n",
                 what, st.w, st.h, box_w, box_h, shown, secs, shown / secs, static_cast<unsigned long long>(st.decoded), st.decode_ms,
                 static_cast<unsigned long long>(st.dropped), static_cast<unsigned long long>(st.shown_dropped), cpu / secs);
    CHECK(shown > static_cast<int>(secs * 20));
}

bool have_tool(const char *t)
{
    return std::system(("command -v " + std::string(t) + " > /dev/null 2>&1").c_str()) == 0;
}

void decoders()
{
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    const std::vector<std::string> js = jpegs();
    if (!av || js.empty() || !TurboJpeg::get().available()) {
        SKIP("playback decoders: needs FFmpeg's libraries, the testsrc fixture (ffmpeg) and libturbojpeg");
        return;
    }
    const std::string d = make_temp_dir("mov");
    CHECK(make_mjpeg_mkv(*av, js, d, "scene_001_nodate", 300)); /* 10 s, 12 different frames looped */
    const std::string f = d + "/scene_001_nodate.mkv";

    /* MJPEG: every frame decodes at the box scale; the exact seek */
    VideoDecoder dec;
    CHECK(dec.open(*av, f, 352, 198, PixFmt::RGB565, &why));
    CHECK(dec.mjpeg());
    CHECK(dec.width() == 1280 && dec.height() == 720);
    CHECK(std::llabs(dec.duration_ms() - 10000) <= 40);
    CHECK(std::fabs(dec.frame_ms() - 33.33) < 0.5);
    Picture p;
    std::int64_t pts = -1;
    int n = 0;
    std::int64_t last = -1;
    bool order = true;
    while (dec.next(p, pts, false) != VideoDecoder::End && n < 400) {
        order = order && pts > last;
        last = pts;
        ++n;
    }
    CHECK_EQ(n, 300);
    CHECK(order);
    CHECK(p.w == 352 && p.h == 198);
    for (std::int64_t target : {5000LL, 1234LL, 9900LL, 0LL}) {
        CHECK(dec.seek(target));
        CHECK_EQ(dec.next(p, pts, false), static_cast<int>(VideoDecoder::Picture_));
        const std::int64_t want = (target * 30 + 500) / 1000 * 1000 / 30; /* the frame at (or nearest) the target */
        CHECK(std::llabs(pts - want) <= 34);
        std::fprintf(stderr, "playback: MJPEG seek to %lld ms lands on the frame at %lld ms\n", static_cast<long long>(target),
                     static_cast<long long>(pts));
    }
    /* the drop rule: late MJPEG frames are skipped before decoding */
    CHECK(dec.seek(0));
    CHECK_EQ(dec.next(p, pts, true), static_cast<int>(VideoDecoder::Skipped));
    CHECK(dec.dropped() >= 1);
    dec.close();

    /* the movie thread: a clock that jumps ahead makes frames drop; a seek empties the presenter and lands there */
    std::atomic<std::int64_t> clk{0};
    Movie mv(*av, [&] { return clk.load(); }, [] { return true; });
    CHECK(mv.open(f, 352, 198, PixFmt::RGB565, &why));
    CHECK(wait_for(2000, [&] { return mv.presenter().full(); }));
    clk = 6000; /* far behind: the next frames are late */
    for (int i = 0; i < 60; ++i) {
        mv.presenter().pick(clk.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(mv.status().dropped > 10);
    mv.seek(8000);
    clk = 8000;
    CHECK(wait_for(2000, [&] { return mv.presenter().next_pts() >= 0; }));
    CHECK(std::llabs(mv.presenter().next_pts() - 8000) <= 34);
    std::int64_t shown_pts = -1;
    CHECK(mv.presenter().pick(8040, &shown_pts) != nullptr);
    CHECK(shown_pts >= 7966 && shown_pts <= 8040);
    mv.close();

    /* the build box's playback numbers (10 s, real time): MJPEG 720p into the canvas box */
    bench(*av, f, "MJPEG 720p", 6.0, 352, 198);

    /* H.264 (FFmpeg's software decoder, own YUV-to-RGB): a fixture made by the build box's ffmpeg (libx264) */
    if (!have_tool("ffmpeg")) {
        SKIP("H.264 playback: ffmpeg MISSING on the build box");
    } else {
        const std::string h = d + "/clip.mkv";
        const std::string cmd = "ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 10 -c:v libx264 -preset ultrafast "
                                "-pix_fmt yuv420p -g 30 '" + h + "' > /dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            SKIP("H.264 playback: the build box's ffmpeg has no libx264 encoder");
        } else {
            VideoDecoder hd;
            CHECK(hd.open(*av, h, 352, 198, PixFmt::XRGB8888, &why));
            CHECK(!hd.mjpeg());
            CHECK_EQ(hd.codec_name(), std::string("H.264"));
            int frames = 0;
            while (hd.next(p, pts, false) != VideoDecoder::End && frames < 400) ++frames;
            CHECK(frames >= 299 && frames <= 300);
            CHECK(p.w == 352 && p.h == 198);
            CHECK(hd.seek(4200)); /* the keyframe before (4.0 s), then decoded forward to the target */
            CHECK_EQ(hd.next(p, pts, false), static_cast<int>(VideoDecoder::Picture_));
            CHECK(pts >= 4167 && pts <= 4234);
            std::fprintf(stderr, "playback: H.264 seek to 4200 ms shows the frame at %lld ms (decoded forward from the keyframe)\n",
                         static_cast<long long>(pts));
            hd.close();
            Picture th;
            CHECK(first_picture(*av, h, 64, 36, PixFmt::RGB565, th, &why)); /* the scenes row's thumbnail */
            CHECK(th.w == 64 && th.h == 36);
            bench(*av, h, "H.264 720p", 6.0, 352, 198);
        }
    }
    /* a file that is not a movie */
    write_file(d + "/bad.mkv", "nothing");
    VideoDecoder bad;
    CHECK(!bad.open(*av, d + "/bad.mkv", 352, 198, PixFmt::RGB565, &why));
    rm_tree(d);
}

/* the controller's playback on the demo folder's fixture "Trip" (10 s, MJPEG 720p with a tone); WSL has no sound device:
 * the engine's output fails and the picture runs on the monotonic clock (as on a board without sound) */
void controller_playback()
{
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (!av || !TurboJpeg::get().can_encode()) {
        SKIP("controller playback: needs FFmpeg's libraries and libturbojpeg");
        return;
    }
    const std::string d = make_temp_dir("cpl");
    ControllerConfig c;
    c.fake = true;
    c.fake_opt.frames = 2;
    c.persist = false;
    c.log_every = 1000;
    c.ui_tid = current_tid();
    c.folder = d + "/VHS";
    c.soc_override = "bcm2837";
    c.clock_synced = 0;
    c.utc = true;
    c.demo_scenes = "1";
    Controller ctl(c);
    CHECK(wait_for(20000, [&] { return ctl.step(mono_ms()).scene_count == 14; }));
    const std::string trip = "Trip_013_20261114-190500.mkv";
    ctl.playback_open(trip, 352, 198);
    PlaybackView v = ctl.playback();
    CHECK(v.open && v.playing && v.error.empty());
    CHECK_EQ(v.title, std::string("Trip_013_20261114-190500"));
    int shown = 0;
    CHECK(wait_for(3000, [&] {
        if (ctl.playback_picture()) ++shown;
        ctl.step(mono_ms());
        return ctl.playback().pos_ms >= 1500;
    }));
    v = ctl.playback();
    CHECK(std::llabs(v.duration_ms - 10000) <= 100);
    CHECK_EQ(v.codec, std::string("MJPEG"));
    CHECK(shown >= 10);
    /* pause: the clock stops */
    ctl.playback_toggle();
    ctl.step(mono_ms());
    const std::int64_t p0 = ctl.playback().pos_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ctl.step(mono_ms());
    CHECK(!ctl.playback().playing);
    CHECK(std::llabs(ctl.playback().pos_ms - p0) <= 5);
    /* seek while paused: exactly there, and the picture of that frame comes */
    ctl.playback_seek_to(5000);
    CHECK(wait_for(1500, [&] {
        ctl.step(mono_ms());
        return std::llabs(ctl.playback().pos_ms - 5000) <= 40;
    }));
    CHECK(wait_for(2000, [&] { return ctl.playback_picture() != nullptr; }));
    ctl.playback_seek_by(-5000);
    CHECK(wait_for(1500, [&] {
        ctl.step(mono_ms());
        return ctl.playback().pos_ms <= 40;
    }));
    ctl.playback_seek_by(30000); /* Shift+Right past the end: the last frame */
    CHECK(wait_for(1500, [&] {
        ctl.step(mono_ms());
        return ctl.playback().pos_ms >= 9900;
    }));
    /* play to the end: paused there; Play starts again from the beginning */
    ctl.playback_seek_to(9000);
    ctl.playback_toggle();
    CHECK(wait_for(4000, [&] {
        ctl.playback_picture();
        ctl.step(mono_ms());
        return ctl.playback().ended;
    }));
    CHECK(!ctl.playback().playing);
    ctl.playback_toggle();
    CHECK(wait_for(1500, [&] {
        ctl.step(mono_ms());
        return ctl.playback().playing && ctl.playback().pos_ms < 800;
    }));
    ctl.playback_close();
    CHECK(!ctl.playback().open);
    const MovieStatus st = ctl.last_playback();
    CHECK(st.decoded > 30);
    std::fprintf(stderr, "controller playback: %llu decoded (%.1f ms each), %llu dropped late, %llu not shown\n",
                 static_cast<unsigned long long>(st.decoded), st.decode_ms, static_cast<unsigned long long>(st.dropped),
                 static_cast<unsigned long long>(st.shown_dropped));
    /* a file that is not a movie: the toast */
    write_file(d + "/VHS/broken.mkv", "not a movie");
    ctl.playback_open("broken.mkv", 352, 198);
    CHECK(!ctl.playback().error.empty());
    CHECK_EQ(ctl.step(mono_ms()).notice.text, std::string(kNoticeCannotPlay));
    ctl.playback_close();
    ctl.stop();
    rm_tree(d);
}

} // namespace

void test_movie()
{
    presenter();
    yuv();
    layout();
    decoders();
    controller_playback();
}
