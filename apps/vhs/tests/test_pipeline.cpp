/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the whole pipeline on the fake camera: pictures of the whole frame at the fit size (letterbox and
 * pillarbox), both pixel formats, a forced scale, the box change, the drop counters when nobody takes the pictures, the
 * stats interval and its log cadence, MJPEG without TurboJPEG (failures counted, the reason kept), the chooser at every
 * open, and stop / start (the camera released and opened again).
 */
#include "check.hpp"

#include "camera/fake_camera.hpp"
#include "jpeg/turbojpeg.hpp"
#include "live/clock.hpp"
#include "live/pipeline.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace vhs;

namespace {

struct Rig {
    explicit Rig(FakeOptions o = small()) : cam(o), finder(make_fake_finder(cam)) {}
    static FakeOptions small()
    {
        FakeOptions o;
        o.frames = 2;
        return o;
    }
    PipelineConfig config(const char *mode)
    {
        PipelineConfig c;
        c.api = &cam;
        c.finder = finder.get();
        if (mode) parse_mode(mode, c.mode);
        c.box_w = 352;
        c.box_h = 198;
        c.log_stats = false;
        c.stats_ms = 200;
        c.poll_ms = 50;
        c.retry_ms = 100;
        c.ui_tid = current_tid();
        return c;
    }
    FakeCamera cam;
    std::unique_ptr<CameraFinder> finder;
};

bool take_one(Pipeline &p, int ms)
{
    return wait_for(ms, [&p] { return p.pictures().take(); });
}

/* what LVGL checks before it draws a picture (lv_draw_buf_init): stride x height bytes */
bool lvgl_can_draw(const Picture &pic)
{
    return pic.w > 0 && pic.h > 0 && pic.stride >= pic.w * pix_bytes(pic.fmt) &&
           pic.px.size() >= static_cast<std::size_t>(pic.stride) * pic.h;
}

void yuyv_pictures()
{
    for (PixFmt fmt : {PixFmt::RGB565, PixFmt::XRGB8888}) {
        Rig r;
        PipelineConfig c = r.config("YUYV:640x360:30");
        c.fmt = fmt;
        Pipeline p(c);
        p.start();
        CHECK(take_one(p, 3000));
        const Picture &pic = p.pictures().front();
        CHECK(pic.fmt == fmt);
        CHECK_EQ(pic.dec_w, 640);
        CHECK_EQ(pic.w, 352); /* 16:9 in 16:9: the whole box */
        CHECK_EQ(pic.h, 198);
        CHECK_EQ(pic.stride, 352 * pix_bytes(fmt));
        CHECK_EQ(pic.px.size(), static_cast<std::size_t>(352 * 198 * pix_bytes(fmt)));
        CHECK(lvgl_can_draw(pic));
        CHECK_EQ(pic.mode.fourcc, kYUYV);
        CHECK(pic.done_us >= pic.dq_us);
        CHECK(pic.dq_us >= pic.ts_us);
        CHECK(pic.n >= 1);
        CHECK_EQ(pic.gen, 1ull);
        /* the whole frame: the top-left pixel is the test card's first bar (white), not a crop of the middle */
        if (fmt == PixFmt::XRGB8888) {
            CHECK(pic.px[0] > 180);
            CHECK(pic.px[1] > 180);
            CHECK(pic.px[2] > 180);
            CHECK_EQ(static_cast<int>(pic.px[3]), 0xFF);
        } else {
            std::uint16_t v;
            std::memcpy(&v, pic.px.data(), 2);
            CHECK((v >> 11) > 22);
            CHECK((v & 0x1F) > 22);
        }
        p.stop();
    }
}

void box_and_pillarbox()
{
    Rig r;
    Pipeline p(r.config("YUYV:640x480:30"));
    CHECK_EQ(p.scale(), 0);
    p.set_scale(3);
    CHECK_EQ(p.scale(), 0); /* not a scale: the fit rule */
    p.set_box(288, 162);
    p.start();
    CHECK(wait_for(3000, [&p] { return p.pictures().take() && p.pictures().front().box_w == 288; }));
    const Picture &pic = p.pictures().front();
    CHECK_EQ(pic.h, 162); /* 4:3 in a 16:9 box: pillarboxed, never cropped */
    CHECK_EQ(pic.w, 216);
    CHECK(place_in_box(pic.w, pic.h, 288, 162) == (Rect{36, 0, 216, 162}));
    p.stop();
}

void drops()
{
    Rig r;
    Pipeline p(r.config("YUYV:640x360:30"));
    p.start();
    /* nobody takes the pictures: they are dropped in the picture mailbox, decoding goes on */
    CHECK(wait_for(3000, [&p] { return p.decoded() >= 8; }));
    CHECK(p.pictures().drops() >= 6);
    p.stop();
}

void intervals()
{
    Rig r;
    PipelineConfig c = r.config("YUYV:640x360:30");
    c.keep_totals = true;
    Pipeline p(c);
    int seen = 0;
    p.set_on_interval([&seen](const Interval &) { ++seen; });
    p.start();
    const std::int64_t t0 = mono_us();
    while (mono_us() - t0 < 900000) {
        if (p.pictures().take()) {
            p.note_shown(p.pictures().front());
            p.note_refresh(2.0, 1.0, 40.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    CHECK(p.has_interval());
    const Interval iv = p.last_interval();
    p.stop();
    CHECK(seen >= 3);
    CHECK(iv.index >= 3);
    CHECK_EQ(iv.state, std::string("streaming"));
    CHECK_EQ(iv.mode, std::string("YUYV 640x360 30 fps"));
    CHECK_EQ(iv.fmt, std::string("RGB565"));
    CHECK_EQ(iv.box, std::string("352x198"));
    CHECK(iv.cap_fps > 15 && iv.cap_fps < 45);
    CHECK(iv.dec_fps > 10);
    CHECK(iv.shown_fps > 5);
    CHECK(iv.conv_ms.n > 0);
    CHECK_EQ(iv.ui_ms.mean, 2.0);
    CHECK_EQ(iv.render_ms.mean, 1.0);
    CHECK_EQ(iv.lat_ms.mean, 40.0);
    CHECK(iv.cores >= 0);
    CHECK(iv.rss_mb > 0);
    CHECK(iv.scale.find("1/1 640x360>352x198") != std::string::npos);
    CHECK(!iv.under_voltage()); /* the build box has neither source */
    const std::string line = format_log(iv);
    CHECK(line.compare(0, 11, "[vhs] stats") == 0);
    CHECK(line.find("cap=") != std::string::npos);
    const Totals t = p.totals();
    CHECK(t.intervals >= 2); /* the first is the warm-up */
    CHECK(t.cap > 0);
    CHECK(!t.conv_ms.empty());
    CHECK(t.have_cores);
}

void mjpeg()
{
    for (PixFmt fmt : {PixFmt::RGB565, PixFmt::XRGB8888}) {
        Rig r;
        PipelineConfig c = r.config("MJPG:1280x720:30");
        c.fmt = fmt;
        Pipeline p(c);
        p.start();
        if (TurboJpeg::get().available()) {
            CHECK(take_one(p, 5000));
            const Picture &pic = p.pictures().front();
            CHECK_EQ(pic.denom, 2);
            CHECK_EQ(pic.dec_w, 640);
            CHECK_EQ(pic.dec_h, 360);
            CHECK_EQ(pic.w, 352); /* resampled to the box: the whole frame */
            CHECK_EQ(pic.h, 198);
            CHECK(lvgl_can_draw(pic));
            CHECK_EQ(pic.stride, 352 * pix_bytes(fmt));
            CHECK_EQ(p.decode_failures(), 0ull);
            /* a box the decode lands on exactly: 1/2 of 1280x720 = 640x360, decoded straight into the picture */
            p.set_box(640, 360);
            CHECK(wait_for(3000, [&p] { return p.pictures().take() && p.pictures().front().box_w == 640; }));
            CHECK_EQ(p.pictures().front().w, 640);
            CHECK_EQ(p.pictures().front().denom, 2);
            /* a forced 1/8 (the bench's --scale): 160x90 decoded, resampled up to the fit size */
            p.set_scale(8);
            CHECK(wait_for(3000, [&p] { return p.pictures().take() && p.pictures().front().denom == 8; }));
            CHECK_EQ(p.pictures().front().dec_w, 160);
            CHECK_EQ(p.pictures().front().w, 640);
        } else {
            /* no TurboJPEG: the frames still flow, every decode fails with the reason */
            CHECK(wait_for(3000, [&p] { return p.decode_failures() >= 3; }));
            CHECK(p.captured() >= 3);
            CHECK_EQ(p.decoded(), 0ull);
            CHECK(p.last_decode_error().find("libturbojpeg0") != std::string::npos);
            CHECK(!p.decoder_ok());
            if (fmt == PixFmt::RGB565)
                SKIP("MJPEG decode: no libturbojpeg.so.0 (tests/run.sh points VHS_TURBOJPEG at a private copy)");
        }
        p.stop();
    }
}

void chooser_and_restart()
{
    Rig r;
    PipelineConfig c = r.config(nullptr);
    int calls = 0;
    c.choose = [&calls](const std::vector<CamMode> &listed, const CamIdentity &id) {
        ++calls;
        ModeReq m;
        parse_mode("YUYV:640x480:30", m);
        (void)listed;
        (void)id;
        return m;
    };
    Pipeline p(c);
    p.start();
    CHECK(wait_for(3000, [&p] { return p.camera().state == CamState::Streaming; }));
    CHECK_EQ(mode_arg(p.camera().mode), std::string("YUYV:640x480:30"));
    CHECK_EQ(calls, 1);
    /* an explicit mode wins over the chooser */
    ModeReq m;
    parse_mode("YUYV:640x360:30", m);
    p.set_mode(m);
    CHECK(wait_for(3000, [&p] { return mode_arg(p.camera().mode) == "YUYV:640x360:30"; }));
    /* stop releases the camera; start opens it again */
    p.stop();
    CHECK(!r.cam.is_open());
    CHECK(!p.running());
    p.start();
    CHECK(wait_for(3000, [&p] { return p.camera().state == CamState::Streaming && p.camera().gen >= 3; }));
    CHECK(r.cam.is_open());
    p.stop();
}

} // namespace

void test_pipeline()
{
    yuyv_pictures();
    box_and_pillarbox();
    drops();
    intervals();
    mjpeg();
    chooser_and_restart();
}
