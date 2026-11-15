/*
 * SPDX-License-Identifier: MIT
 *
 * VHS tests: the JPEG side. Without a library: the marker reader (size, DHT) and the .mjpg splitter. With the real
 * libturbojpeg (the build box's, or tests/run.sh's private copy through VHS_TURBOJPEG): encode the test card,
 * decode it at 1/1, 1/2, 1/4, 1/8, a damaged frame, and an ffmpeg-made MJPEG fixture ($VHS_FIXTURES/testsrc-720.mjpg)
 * through the fake camera and the whole pipeline.
 */
#include "check.hpp"

#include "live/clock.hpp"
#include "camera/fake_camera.hpp"
#include "live/pipeline.hpp"
#include "jpeg/turbojpeg.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

using namespace vhs;

namespace {

std::vector<std::uint8_t> fake_jpeg(int w, int h, bool dht, const std::vector<std::uint8_t> &scan)
{
    std::vector<std::uint8_t> j = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43};
    j.insert(j.end(), 65, 1); /* a DQT body */
    const std::uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, static_cast<std::uint8_t>(h >> 8), static_cast<std::uint8_t>(h),
                                static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w), 0x03, 1, 0x21, 0, 2, 0x11,
                                1, 3, 0x11, 1};
    j.insert(j.end(), sof, sof + sizeof sof);
    if (dht) {
        const std::uint8_t d[] = {0xFF, 0xC4, 0x00, 0x05, 0x00, 0x00, 0x00};
        j.insert(j.end(), d, d + sizeof d);
    }
    const std::uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 1, 0x00, 2, 0x11, 3, 0x11, 0x00, 0x3F, 0x00};
    j.insert(j.end(), sos, sos + sizeof sos);
    j.insert(j.end(), scan.begin(), scan.end());
    j.push_back(0xFF);
    j.push_back(0xD9);
    return j;
}

void markers()
{
    int w = 0, h = 0;
    bool dht = false;
    std::vector<std::uint8_t> a = fake_jpeg(1920, 1080, true, {1, 2, 0xFF, 0x00, 3});
    CHECK(jpeg_peek(a.data(), a.size(), w, h, dht));
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1080);
    CHECK(dht);
    std::vector<std::uint8_t> b = fake_jpeg(640, 480, false, {9, 9, 9});
    CHECK(jpeg_peek(b.data(), b.size(), w, h, dht));
    CHECK_EQ(w, 640);
    CHECK(!dht); /* a camera without Huffman tables would need the Annex K tables inserted (P1 core) */
    const std::uint8_t junk[] = {0x00, 0x11, 0x22, 0x33, 0x44};
    CHECK(!jpeg_peek(junk, sizeof junk, w, h, dht));
    CHECK(!jpeg_peek(nullptr, 0, w, h, dht));
    const std::uint8_t cut[] = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 1, 2};
    CHECK(!jpeg_peek(cut, sizeof cut, w, h, dht));

    /* two frames with junk around them and a stuffed 0xFF 0x00 in the scan, then a frame cut short */
    std::vector<std::uint8_t> stream = {0x00, 0x01};
    stream.insert(stream.end(), a.begin(), a.end());
    stream.push_back(0x55);
    stream.insert(stream.end(), b.begin(), b.end());
    stream.insert(stream.end(), a.begin(), a.begin() + 40);
    const auto frames = split_mjpeg(stream);
    CHECK_EQ(frames.size(), static_cast<std::size_t>(2));
    if (frames.size() == 2) {
        CHECK(frames[0] == a);
        CHECK(frames[1] == b);
    }
    CHECK(split_mjpeg({}).empty());
}

void real_library()
{
    TurboJpeg &tj = TurboJpeg::get();
    if (!tj.available()) {
        SKIP("TurboJPEG decode/encode: no libturbojpeg.so.0 on the build box");
        CHECK(tj.error().find("libturbojpeg0") != std::string::npos);
        JpegDecoder d;
        CHECK(!d.ok());
        JpegInfo info;
        CHECK(!d.header(nullptr, 0, info));
        return;
    }
    CHECK(tj.can_encode());
    std::vector<std::uint8_t> card;
    fake_test_card(640, 360, 0, 2, card);
    JpegEncoder enc;
    CHECK(enc.ok());
    std::vector<std::uint8_t> jpeg;
    CHECK(enc.encode_bgrx(card.data(), 640, 360, 85, jpeg));
    int w = 0, h = 0;
    bool dht = false;
    CHECK(jpeg_peek(jpeg.data(), jpeg.size(), w, h, dht));
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 360);
    CHECK(dht); /* baseline TurboJPEG output carries its tables, as the camera's frames do (P0-a) */
    JpegDecoder dec;
    CHECK(dec.ok());
    JpegInfo info;
    CHECK(dec.header(jpeg.data(), jpeg.size(), info));
    CHECK_EQ(info.w, 640);
    CHECK_EQ(info.subsamp, 1); /* 4:2:2 */
    for (int d : kDenoms) {
        const int dw = tj_scaled(640, d), dh = tj_scaled(360, d);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4, 0);
        CHECK_EQ(dec.decode_bgrx(jpeg.data(), jpeg.size(), d, out.data(), dw * 4, true), 0);
        /* (x 1/16, y 1/4 of the picture): the first bar, white (235 +- noise) */
        const std::size_t at = (static_cast<std::size_t>(dh / 4) * dw + dw / 16) * 4;
        CHECK(out[at] > 200);
        CHECK(out[at + 1] > 200);
        CHECK(out[at + 2] > 200);
        CHECK_EQ(static_cast<int>(out[at + 3]), 0xFF); /* X is 0xFF: LVGL's XRGB8888 as it is */
    }
    CHECK_EQ(tj_scaled(1080, 8), 135);
    /* accurate DCT decodes too */
    std::vector<std::uint8_t> out(640 * 360 * 4);
    CHECK_EQ(dec.decode_bgrx(jpeg.data(), jpeg.size(), 1, out.data(), 640 * 4, false), 0);
    /* garbage and a frame cut in half */
    const std::uint8_t junk[] = {0xFF, 0xD8, 0x00, 0x01, 0x02};
    CHECK_EQ(dec.decode_bgrx(junk, sizeof junk, 1, out.data(), 640 * 4, true), -1);
    CHECK(!dec.last_error().empty());
    const int half = dec.decode_bgrx(jpeg.data(), jpeg.size() / 2, 1, out.data(), 640 * 4, true);
    CHECK(half == 1 || half == -1); /* a warning (decoded with grey) or an error, never a crash */

    /* informational: the build box's decode cost of the 720p card */
    std::vector<std::uint8_t> c720, j720;
    fake_test_card(1280, 720, 1, 2, c720);
    enc.encode_bgrx(c720.data(), 1280, 720, 85, j720);
    for (int d : {1, 2, 4}) {
        const int dw = tj_scaled(1280, d), dh = tj_scaled(720, d);
        std::vector<std::uint8_t> o(static_cast<std::size_t>(dw) * dh * 4);
        const std::int64_t t0 = mono_us();
        for (int i = 0; i < 20; ++i) dec.decode_bgrx(j720.data(), j720.size(), d, o.data(), dw * 4, true);
        std::fprintf(stderr, "info: build box TurboJPEG 1280x720 card (%zu KB) at 1/%d: %.2f ms\n", j720.size() / 1024, d,
                     static_cast<double>(mono_us() - t0) / 20000.0);
    }
}

void fixture_file()
{
    const std::string path = fixture("testsrc-720.mjpg");
    if (path.empty()) {
        SKIP("MJPEG fixture: no $VHS_FIXTURES/testsrc-720.mjpg (tests/run.sh makes it with ffmpeg)");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto frames = split_mjpeg(bytes);
    CHECK(frames.size() >= 5);
    bool all = true;
    for (const auto &fr : frames) {
        int w = 0, h = 0;
        bool dht = false;
        all = all && jpeg_peek(fr.data(), fr.size(), w, h, dht) && w == 1280 && h == 720 && dht;
    }
    CHECK(all);

    FakeOptions o;
    o.canned_file = path;
    FakeCamera cam(o);
    CHECK(cam.canned_error().empty());
    const std::vector<CamMode> modes = cam.modes();
    CHECK_EQ(modes.size(), static_cast<std::size_t>(3));
    if (!modes.empty()) {
        CHECK_EQ(modes[0].fourcc, kMJPG);
        CHECK_EQ(modes[0].w, 1280);
    }
    if (!TurboJpeg::get().available()) {
        SKIP("MJPEG fixture decode: no libturbojpeg.so.0");
        return;
    }
    JpegDecoder dec;
    std::vector<std::uint8_t> out(640 * 360 * 4);
    int ok = 0;
    for (const auto &fr : frames) ok += dec.decode_bgrx(fr.data(), fr.size(), 2, out.data(), 640 * 4, true) == 0;
    CHECK_EQ(ok, static_cast<int>(frames.size()));

    std::unique_ptr<CameraFinder> finder = make_fake_finder(cam);
    PipelineConfig c;
    c.api = &cam;
    c.finder = finder.get();
    c.log_stats = false;
    c.ui_tid = current_tid();
    c.poll_ms = 50;
    c.retry_ms = 100;
    Pipeline p(c);
    p.start();
    CHECK(wait_for(5000, [&p] { return p.pictures().take(); }));
    const Picture &pic = p.pictures().front();
    CHECK_EQ(pic.mode.w, 1280);
    CHECK_EQ(pic.denom, 2);
    CHECK_EQ(pic.w, 352); /* 640x360 resampled to the whole box */
    CHECK_EQ(pic.h, 198);
    CHECK(wait_for(3000, [&p] { return p.decoded() >= 10; }));
    CHECK_EQ(p.decode_failures(), 0ull);
    p.stop();

    FakeOptions bad;
    bad.canned_file = "/nonexistent/x.mjpg";
    FakeCamera none(bad);
    CHECK(!none.canned_error().empty());
    CHECK(none.modes().size() >= 5); /* back to the synthetic card */
}

} // namespace

void test_turbojpeg()
{
    markers();
    real_library();
    fixture_file();
}
