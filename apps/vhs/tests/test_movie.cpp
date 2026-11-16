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
#include <cstdlib>
#include <cstring>
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
    auto b = pr.pick(70, &pts); /* two due, the older one only a little late (G6): it is shown, the newer waits */
    CHECK(b && b->w == 2 && pts == 33);
    CHECK_EQ(pr.dropped(), 0ull);
    b = pr.pick(75, &pts);
    CHECK(b && b->w == 3 && pts == 67);
    CHECK_EQ(pr.size(), static_cast<std::size_t>(0));
    /* more than two periods late while a newer one is due: dropped */
    pr.set_late_ms(67);
    pr.push({pic(5), 100});
    pr.push({pic(6), 133});
    pr.push({pic(7), 167});
    b = pr.pick(240, &pts); /* 100 and 133 are more than 67 ms late, 167 is not */
    CHECK(b && b->w == 7 && pts == 167);
    CHECK_EQ(pr.dropped(), 2ull);
    pr.push({pic(8), 200});
    b = pr.pick(400, &pts); /* the only one: shown however late */
    CHECK(b && b->w == 8);
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

    /* G6: steered toward the sound: a small difference is taken out slowly (at most 5 %), a big one at once */
    {
        PlayClock s;
        s.set(0, 0);
        s.resume(0);
        CHECK(!s.steer(1060, 1000)); /* 60 ms behind the sound: 4 % faster */
        CHECK(std::fabs(s.rate() - 1.04) < 1e-9);
        CHECK_EQ(s.at(1000), 1000ll);
        CHECK_EQ(s.at(2000), 2040ll);
        CHECK(!s.steer(1900, 2000)); /* 140 ms ahead: at most 5 % slower */
        CHECK(std::fabs(s.rate() - 0.95) < 1e-9);
        CHECK(s.steer(5000, 2100)); /* a seek of the sound: moved at once, at real time */
        CHECK_EQ(s.at(2100), 5000ll);
        CHECK(std::fabs(s.rate() - 1.0) < 1e-9);
        /* a sound position that comes in 250 ms steps (held, then jumping): the picture's clock never stands still
         * and never jumps; it stays within 140 ms of the true position */
        PlayClock v;
        v.set(0, 10000);
        v.resume(10000);
        std::int64_t last = -1, worst = 0;
        bool smooth = true;
        for (std::int64_t t = 10000; t <= 30000; t += 33) {
            if ((t - 10000) % 264 == 0) v.steer((t - 10000) / 250 * 250, t); /* the sound's steppy position, sampled */
            const std::int64_t at = v.at(t);
            if (last >= 0 && (at - last < 25 || at - last > 41)) smooth = false;
            last = at;
            worst = std::max<std::int64_t>(worst, std::llabs(at - (t - 10000)));
        }
        CHECK(smooth);
        CHECK(worst <= 200);
    }

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
    /* G6: drops while the toast shows (4 s) do not count; a new 2 s streak after it hid fires again */
    for (std::int64_t t = 3900; t < 7700; t += 100) CHECK(!w.drop(t));
    CHECK(!w.drop(7800)); /* the streak starts again here */
    bool again = false;
    for (std::int64_t t = 7900; t <= 9800 && !again; t += 100) again = w.drop(t);
    CHECK(again);
    CHECK_EQ(w.fires(), 2);
    /* G6, the toast as the controller drives it (play_step every 33 ms, NoticeQueue's 4 s): drops all the time from 0
     * show it at 2 s, hide it at 6 s, show it again at 8 s (a new 2 s after it hid); paused (no drops) it still hides */
    {
        DropWindow d;
        NoticeQueue q(4000);
        std::int64_t first = -1, hid = -1, second = -1;
        for (std::int64_t t = 0; t <= 12000; t += 33) {
            if (d.drop(t)) q.push(Notice{kNoticeDrops, false});
            const bool on = q.current(t).text == kNoticeDrops;
            if (on && first < 0) first = t;
            if (!on && first >= 0 && hid < 0) hid = t;
            if (on && hid >= 0 && second < 0) second = t;
        }
        CHECK(first >= 2000 && first < 2100);
        CHECK(hid >= first + 4000 && hid < first + 4100);
        CHECK(second >= hid + 2000 && second < hid + 2200);
        /* paused right after it showed: no more drops, it hides after its 4 s all the same */
        DropWindow p;
        NoticeQueue pq(4000);
        std::int64_t on_at = -1, off_at = -1;
        for (std::int64_t t = 0; t <= 9000; t += 33) {
            if (on_at < 0 && p.drop(t)) pq.push(Notice{kNoticeDrops, false});
            const bool on = pq.current(t).text == kNoticeDrops;
            if (on && on_at < 0) on_at = t;
            if (!on && on_at >= 0 && off_at < 0) off_at = t;
        }
        CHECK(on_at >= 0);
        CHECK(off_at >= on_at + 4000 && off_at < on_at + 4100);
    }
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

    /* G6, the drops toast of the harness shots: a screen that takes pictures only every 250 ms (the harness's loop
     * stalls while it shoots and checks the layout) leaves the presenter full, the movie thread decodes late and the
     * drop rule drops: the notice fires after 2 s of that; at the display rate (the bench above) nothing drops */
    {
        PlayClock pc;
        std::mutex pm;
        const std::int64_t s0 = mono_ms();
        pc.set(0, s0);
        pc.resume(s0);
        Movie slow(*av, [&] {
            std::lock_guard<std::mutex> lock(pm);
            return pc.at(mono_ms());
        }, [] { return true; });
        CHECK(slow.open(f, 352, 198, PixFmt::RGB565, &why));
        bool notice = false;
        while (mono_ms() - s0 < 3500) {
            {
                std::lock_guard<std::mutex> lock(pm);
                slow.presenter().pick(pc.at(mono_ms()));
            }
            notice = slow.take_drops_notice() || notice;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        const MovieStatus ss = slow.status();
        slow.close();
        CHECK(ss.dropped > 20);
        CHECK(notice);
        std::fprintf(stderr, "playback: a screen taking pictures every 250 ms: %llu dropped late, notice %s\n",
                     static_cast<unsigned long long>(ss.dropped), notice ? "fired" : "not fired");
    }

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
/* G6, the clock behind the skipping playback of the G5 boards: the app picks a picture every 33 ms (the frame timer) and
 * runs play_step every 250 ms (the status timer). With the sound's position read in play_step and used as the clock,
 * the clock stood still for 250 ms and jumped 250 ms: about half the frames dropped late, a quarter not shown (the
 * harness: 60 dropped late, 36 not shown of 8 s). Now the picture's clock is monotonic and steered: every sound output
 * of the fake (real time, a stepping delay, a sink that drains at once, the null sink) plays with next to no drops. */
void playback_cadence()
{
    std::string why;
    const AvLibs *av = real_av_libs(&why);
    if (!av || !TurboJpeg::get().can_encode()) {
        SKIP("playback cadence: needs FFmpeg's libraries and libturbojpeg");
        return;
    }
    const std::string d = make_temp_dir("cadence");
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
    const char *old = std::getenv("VHS_FAKE_OUTPUT");
    const std::string saved = old ? old : "";
    Controller ctl(c);
    CHECK(wait_for(20000, [&] { return ctl.step(mono_ms()).scene_count == 14; }));
    const struct {
        const char *mode, *clock;
    } runs[] = {{"ok", "steered to the sound"}, {"steppy", "steered to the sound"},
                {"instant", "monotonic ("}, {"dummy", "monotonic (Dummy Output"}};
    for (const auto &r : runs) {
        ::setenv("VHS_FAKE_OUTPUT", r.mode, 1);
        ctl.playback_open("Trip_013_20261114-190500.mkv", 352, 198);
        const std::int64_t t0 = mono_ms();
        std::int64_t next_step = t0;
        while (mono_ms() - t0 < 5000) {
            ctl.playback_picture();
            if (mono_ms() >= next_step) {
                ctl.step(mono_ms());
                next_step += 250;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        const std::string mode = ctl.playback_clock_mode();
        const std::uint64_t shown = ctl.playback_shown();
        const std::int64_t pos = ctl.playback().pos_ms;
        ctl.playback_close();
        const MovieStatus st = ctl.last_playback();
        std::fprintf(stderr, "playback cadence, fake output %s: clock %s at %lld ms, %llu shown, %llu dropped late, %llu not shown\n",
                     r.mode, mode.c_str(), static_cast<long long>(pos), static_cast<unsigned long long>(shown),
                     static_cast<unsigned long long>(st.dropped), static_cast<unsigned long long>(st.shown_dropped));
        CHECK(mode.compare(0, std::strlen(r.clock), r.clock) == 0);
        CHECK(st.dropped <= 3);
        CHECK(st.shown_dropped <= 8);
        CHECK(shown >= 120);                       /* about 150 frames in 5 s at 30 fps */
        CHECK(pos >= 4500 && pos <= 5300);         /* the picture's clock kept real time */
    }
    if (old) ::setenv("VHS_FAKE_OUTPUT", saved.c_str(), 1);
    else ::unsetenv("VHS_FAKE_OUTPUT");
    rm_tree(d);
}

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
    playback_cadence();
}
