// Host tests of RSS Reader's image decoding (src/ui/image_decode, image_codecs.c: the pinned LVGL's lodepng, AnimatedGIF and
// TJpgDec), with LVGL linked: rss-reader-imgtest, built by tests/flows/runner.mk (build.sh host). The fixtures are
// tests/data/ui/img (make them again with the generator in docs/IMAGES.md).
//
//   rss-reader-imgtest                      the checks (decoding, limits, cancel, turns, a mutation fuzz)
//   rss-reader-imgtest --bench FILE W H     decodes FILE into W x H and prints the time and the memory (VmHWM)
//   rss-reader-imgtest --article W H FILE.. the thumbnails of an article one after the other (as the reader does), kept
#include "test_common.hpp"

#include "image_codecs.h"
#include "image_decode.hpp"
#include "image_probe.hpp"

#include "lvgl/lvgl.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

using namespace rssui;
using namespace rssreader;
using namespace rssreader_test;

int g_fail = 0, g_checks = 0;

namespace {

std::string img(const std::string &name) { return fixture("ui/img/" + name); }

/* RGB565 pixel at (x, y) as 8-bit channels */
void px(const DecodedImage &d, int x, int y, int &r, int &g, int &b)
{
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(d.width) + static_cast<std::size_t>(x)) * 2;
    const unsigned v = d.rgb565[i] | d.rgb565[i + 1] << 8;
    r = static_cast<int>((v >> 11) << 3);
    g = static_cast<int>(((v >> 5) & 63) << 2);
    b = static_cast<int>((v & 31) << 3);
}

long status_kb(const char *key)
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(key, 0) == 0) return std::atol(line.c_str() + std::strlen(key));
    return -1;
}

void test_fit()
{
    int w = 0, h = 0;
    fit_size(640, 360, 300, 300, w, h);
    CHECK(w == 300 && h == 169);
    fit_size(640, 360, 2000, 2000, w, h);             // never enlarged
    CHECK(w == 640 && h == 360);
    fit_size(1600, 1000, 608, 160, w, h);             // the height decides
    CHECK(w == 256 && h == 160);
    fit_size(10, 5000, 600, 160, w, h);
    CHECK(w == 1 && h == 160);
    fit_size(0, 0, 100, 100, w, h);
    CHECK(w == 1 && h == 1);
}

void test_decode_fixtures()
{
    DecodedImage d;
    CHECK(decode_image(img("graph.jpg"), 300, 300, 0, d) == DecodeStatus::Ok);
    CHECK(d.width == 300 && d.height == 169 && d.src_width == 640 && d.src_height == 360);
    CHECK(d.rgb565.size() == 300u * 169u * 2u);
    int r = 0, g = 0, b = 0;
    px(d, 300 * 20 / 100, 169 * 11 / 100, r, g, b);    // the orange legend box
    CHECK(r > 200 && g > 90 && g < 150 && b < 90);
    px(d, 300 * 20 / 100, 169 * 90 / 100, r, g, b);    // the green one
    CHECK(r < 120 && g > 170 && b > 90 && b < 150);
    CHECK(decode_image(img("graph.jpg"), 2000, 2000, 0, d) == DecodeStatus::Ok && d.width == 640 && d.height == 360);
    CHECK(decode_image(img("wide.jpg"), 608, 160, 0, d) == DecodeStatus::Ok && d.width == 256 && d.height == 160);
    CHECK(decode_image(img("progressive.jpg"), 300, 300, 0, d) == DecodeStatus::Unsupported && d.rgb565.empty());
    CHECK(decode_image(img("cmyk.jpg"), 300, 300, 0, d) == DecodeStatus::Unsupported);
    CHECK(decode_image(img("huge.jpg"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    CHECK(decode_image(img("huge.png"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    CHECK(decode_image(img("truncated.jpg"), 300, 300, 0, d) == DecodeStatus::Broken);
    const DecodeStatus corrupt = decode_image(img("corrupt.jpg"), 300, 300, 0, d);
    CHECK(corrupt == DecodeStatus::Broken || corrupt == DecodeStatus::Ok);       // garbage after a valid start: never a crash
    CHECK(decode_image(img("notimage.jpg"), 300, 300, 0, d) == DecodeStatus::Broken);
    CHECK(decode_image("", 300, 300, 0, d) == DecodeStatus::Broken);
    CHECK(decode_image(img("pic.webp"), 300, 300, 0, d) == DecodeStatus::Unsupported);
    CHECK(decode_image(img("wide.gif"), 300, 300, 0, d) == DecodeStatus::Unsupported);
    // PNG with alpha: the transparent corner is the background, the opaque blue box blue
    CHECK(decode_image(img("logo.png"), 360, 160, 0xFFFFFF, d) == DecodeStatus::Ok && d.width == 360 && d.height == 160);
    px(d, 2, 2, r, g, b);
    CHECK(r >= 248 && g >= 248 && b >= 248);
    px(d, 200, 30, r, g, b);
    CHECK(r < 50 && g > 100 && g < 125 && b > 190);
    CHECK(decode_image(img("logo.png"), 360, 160, 0x000000, d) == DecodeStatus::Ok);
    px(d, 2, 2, r, g, b);
    CHECK(r == 0 && g == 0 && b == 0);
    // GIF: the first frame with AnimatedGIF (RSS_USE_ANIMATEDGIF), else not supported
    if (rss_gif_available()) {
        CHECK(decode_image(img("anim.gif"), 600, 600, 0, d) == DecodeStatus::Ok && d.width == 240 && d.height == 140);
        px(d, 60, 70, r, g, b);                       // the yellow disc of frame 1
        CHECK(r > 230 && g > 190 && b < 80);
        px(d, 200, 70, r, g, b);                      // background (palette 0)
        CHECK(r < 30 && g < 30 && b < 40);
    } else {
        CHECK(decode_image(img("anim.gif"), 600, 600, 0, d) == DecodeStatus::Unsupported);
        CHECK(decode_check(probe_image(img("anim.gif"))) == DecodeStatus::Unsupported);
    }
    // what decode_check says without decoding
    CHECK(decode_check(probe_image(img("graph.jpg"))) == DecodeStatus::Ok);
    CHECK(decode_check(probe_image(img("progressive.jpg"))) == DecodeStatus::Unsupported);
    CHECK(decode_check(probe_image(img("huge.jpg"))) == DecodeStatus::TooLarge);
    CHECK(decode_check(probe_image(img("pic.webp"))) == DecodeStatus::Unsupported);
    CHECK(decode_check(probe_image(img("notimage.jpg"))) == DecodeStatus::Broken);
}

void test_cancel()
{
    std::atomic<bool> stop{true};
    DecodedImage d;
    CHECK(decode_image(img("wide.jpg"), 300, 300, 0, d, &stop) == DecodeStatus::Cancelled && d.rgb565.empty());
    CHECK(decode_image(img("logo.png"), 300, 300, 0, d, &stop) == DecodeStatus::Cancelled);
}

void test_turns()
{
    const std::string a = img("wide.jpg"), b = img("logo.png"), c = img(rss_gif_available() ? "anim.gif" : "graph.jpg");
    std::atomic<int> ok{0};
    std::vector<std::thread> t;
    for (int i = 0; i < 6; ++i)
        t.emplace_back([&, i] {
            DecodedImage d;
            for (int k = 0; k < 4; ++k)
                if (decode_image(i % 3 == 0 ? a : i % 3 == 1 ? b : c, 320, 200, 0, d) == DecodeStatus::Ok) ++ok;
        });
    for (std::thread &x : t) x.join();
    CHECK_EQ(ok.load(), 24);
    CHECK_EQ(decodes_peak_parallel(), 1);             // they took turns
}

/* Mutations of every decodable fixture: flipped bytes, cut ends, a header that lies. Never a crash (run under ASan). */
void test_fuzz()
{
    std::mt19937 rnd(1234);
    int runs = 0;
    for (const char *name : {"graph.jpg", "logo.png", "anim.gif", "progressive.jpg"}) {     // anim.gif: AnimatedGIF when built in
        const std::string good = img(name);
        for (int k = 0; k < 150; ++k) {
            std::string m = good;
            const int flips = 1 + static_cast<int>(rnd() % 12);
            for (int f = 0; f < flips; ++f) m[rnd() % m.size()] = static_cast<char>(rnd() & 0xFF);
            if (k % 5 == 0) m.resize(rnd() % m.size());
            DecodedImage d;
            const DecodeStatus st = decode_image(m, 200, 200, 0, d);
            if (st == DecodeStatus::Ok) CHECK(d.rgb565.size() == static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height) * 2u);
            ++runs;
        }
    }
    CHECK(runs == 600);
}

int bench(const std::string &path, int w, int h)
{
    std::string bytes;
    if (!read_file(path, bytes, 64u * 1024 * 1024)) return 2;
    const ImageProbe p = probe_image(bytes);
    const long base = status_kb("VmRSS:");
    const auto t0 = std::chrono::steady_clock::now();
    DecodedImage d;
    const DecodeStatus st = decode_image(bytes, w, h, 0, d);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s %s %dx%d (%.1f MP, %zu bytes) -> %dx%d: %s in %.0f ms; RSS before %ld KB, peak %ld KB (+%ld KB), result %zu KB\n",
                path.c_str(), image_format_name(p.format), p.width, p.height, static_cast<double>(p.pixels()) / 1e6, bytes.size(), d.width,
                d.height, decode_status_name(st), ms, base, status_kb("VmHWM:"), status_kb("VmHWM:") - base, d.rgb565.size() / 1024);
    return st == DecodeStatus::Ok ? 0 : 1;
}

int article(int w, int h, const std::vector<std::string> &files)
{
    const long base = status_kb("VmRSS:");
    std::vector<DecodedImage> kept;
    std::size_t total = 0;
    for (const std::string &f : files) {
        std::string bytes;
        if (!read_file(f, bytes, 64u * 1024 * 1024)) return 2;
        kept.emplace_back();
        decode_image(bytes, w, h, 0, kept.back());
        total += kept.back().rgb565.size();
    }
    std::printf("article of %zu images -> thumbnails %dx%d: kept %zu KB; RSS before %ld KB, after %ld KB, peak %ld KB (+%ld KB)\n", files.size(), w,
                h, total / 1024, base, status_kb("VmRSS:"), status_kb("VmHWM:"), status_kb("VmHWM:") - base);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    lv_init();          // lodepng allocates its output through LVGL's draw buffers
    if (argc >= 5 && std::string(argv[1]) == "--bench") return bench(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    if (argc >= 5 && std::string(argv[1]) == "--article") return article(std::atoi(argv[2]), std::atoi(argv[3]), std::vector<std::string>(argv + 4, argv + argc));
    test_fit();
    test_decode_fixtures();
    test_cancel();
    test_turns();
    test_fuzz();
    std::printf("imgtest: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
