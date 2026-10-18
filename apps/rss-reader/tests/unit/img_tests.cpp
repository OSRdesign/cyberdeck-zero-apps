// Host tests of RSS Reader's image decoding (src/ui/image_decode, image_codecs.c: stb_image, third_party/stb): rss-reader-imgtest,
// built by tests/flows/runner.mk (build.sh host). The fixtures are tests/data/ui/img (made by tools/make_img_fixtures.py).
//
//   rss-reader-imgtest                      the checks (decoding, limits, cancel, turns, a mutation fuzz of 600 files per format;
//                                           RSS_FUZZ_N=n for n per format)
//   rss-reader-imgtest --bench FILE W H     decodes FILE into W x H and prints the time and the memory (VmHWM, and the decoder's
//                                           own count of its allocations)
//   rss-reader-imgtest --article W H FILE.. the thumbnails of an article one after the other (as the reader does), kept
#include "test_common.hpp"

#include "image_codecs.h"
#include "image_decode.hpp"
#include "image_probe.hpp"

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

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

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
    CHECK(decode_image(img("baseline444.jpg"), 64, 64, 0, d) == DecodeStatus::Ok && d.width == 64 && d.height == 40);
    // progressive JPEG: decoded for real (the sky gradient of photo(): blue at the top, the hills dark green at the bottom)
    CHECK(decode_image(img("progressive.jpg"), 300, 300, 0, d) == DecodeStatus::Ok && d.width == 300 && d.height == 200 && d.src_width == 480 &&
          d.src_height == 320);
    px(d, 150, 3, r, g, b);
    CHECK(near(r, 70, 24) && near(g, 120, 24) && near(b, 200, 24));
    px(d, 5, 195, r, g, b);
    CHECK(g > r && g > b && g < 140);
    // CMYK / YCCK (stb_image converts it): the sky stays blue
    CHECK(decode_image(img("cmyk.jpg"), 300, 300, 0, d) == DecodeStatus::Ok && d.src_width == 400);
    px(d, 150, 3, r, g, b);
    CHECK(b > r && b > 140);
    // headers that lie or say too much
    CHECK(decode_image(img("huge.jpg"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    CHECK(decode_image(img("huge-prog.jpg"), 300, 300, 0, d) == DecodeStatus::TooLarge);      // 12 MP progressive: over its 6 MP limit
    CHECK(decode_image(img("huge.png"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    CHECK(decode_image(img("huge.gif"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    CHECK(decode_image(img("huge.bmp"), 300, 300, 0, d) == DecodeStatus::TooLarge);
    // cut and broken files of every format: an error or the rows that arrived, never a crash
    for (const char *name : {"truncated.jpg", "truncated-prog.jpg", "truncated.png", "truncated.gif", "truncated.bmp"}) {
        const DecodeStatus st = decode_image(img(name), 300, 300, 0, d);
        CHECK(st == DecodeStatus::Broken || st == DecodeStatus::Ok);
        if (st != DecodeStatus::Ok) CHECK(d.rgb565.empty());
    }
    CHECK(decode_image(img("truncated.png"), 300, 300, 0, d) == DecodeStatus::Broken);
    for (const char *name : {"corrupt.jpg", "corrupt-prog.jpg", "corrupt.png", "corrupt.gif", "corrupt.bmp"}) {
        const DecodeStatus st = decode_image(img(name), 300, 300, 0, d);
        CHECK(st == DecodeStatus::Broken || st == DecodeStatus::Ok);
    }
    CHECK(decode_image(img("corrupt.png"), 300, 300, 0, d) == DecodeStatus::Broken);
    CHECK(decode_image(img("notimage.jpg"), 300, 300, 0, d) == DecodeStatus::Broken);
    CHECK(decode_image("", 300, 300, 0, d) == DecodeStatus::Broken);
    CHECK(decode_image(img("pic.webp"), 300, 300, 0, d) == DecodeStatus::Unsupported);
    // PNG with alpha: the transparent corner is the background, the opaque blue box blue
    CHECK(decode_image(img("logo.png"), 360, 160, 0xFFFFFF, d) == DecodeStatus::Ok && d.width == 360 && d.height == 160);
    px(d, 2, 2, r, g, b);
    CHECK(r >= 248 && g >= 248 && b >= 248);
    px(d, 200, 30, r, g, b);
    CHECK(r < 50 && g > 100 && g < 125 && b > 190);
    CHECK(decode_image(img("logo.png"), 360, 160, 0x000000, d) == DecodeStatus::Ok);
    px(d, 2, 2, r, g, b);
    CHECK(r == 0 && g == 0 && b == 0);
    // GIF: the first frame only (the yellow disc of frame 1; frame 2's disc is not there)
    CHECK(decode_image(img("anim.gif"), 600, 600, 0, d) == DecodeStatus::Ok && d.width == 240 && d.height == 140);
    px(d, 60, 70, r, g, b);
    CHECK(r > 230 && g > 190 && b < 80);
    px(d, 200, 70, r, g, b);                          // background (palette 0)
    CHECK(r < 30 && g < 30 && b < 40);
    // a GIF wider than 480 (AnimatedGIF refused it) is fine now
    CHECK(decode_image(img("wide.gif"), 300, 300, 0, d) == DecodeStatus::Ok && d.width == 300 && d.height == 60);
    // a static GIF with a transparent colour: over white, then over black
    CHECK(decode_image(img("still.gif"), 160, 100, 0xFFFFFF, d) == DecodeStatus::Ok && d.width == 160 && d.height == 100);
    px(d, 2, 2, r, g, b);
    CHECK(r >= 248 && g >= 248 && b >= 248);
    px(d, 80, 40, r, g, b);                           // the yellow disc
    CHECK(r > 230 && g > 190 && b < 80);
    CHECK(decode_image(img("still.gif"), 160, 100, 0x000000, d) == DecodeStatus::Ok);
    px(d, 2, 2, r, g, b);
    CHECK(r == 0 && g == 0 && b == 0);
    // BMP: photo(200, 130): the sky blue at the top
    CHECK(decode_image(img("pic.bmp"), 200, 200, 0, d) == DecodeStatus::Ok && d.width == 200 && d.height == 130);
    px(d, 100, 2, r, g, b);
    CHECK(near(r, 70, 16) && near(g, 120, 16) && near(b, 200, 16));
}

void test_check()
{
    // what decode_check says without decoding
    CHECK(decode_check(probe_image(img("graph.jpg"))) == DecodeStatus::Ok);
    CHECK(decode_check(probe_image(img("progressive.jpg"))) == DecodeStatus::Ok);
    CHECK(decode_check(probe_image(img("anim.gif"))) == DecodeStatus::Ok);
    CHECK(decode_check(probe_image(img("pic.bmp"))) == DecodeStatus::Ok);
    CHECK(decode_check(probe_image(img("huge.jpg"))) == DecodeStatus::TooLarge);
    CHECK(decode_check(probe_image(img("huge-prog.jpg"))) == DecodeStatus::TooLarge);
    CHECK(decode_check(probe_image(img("pic.webp"))) == DecodeStatus::Unsupported);
    CHECK(decode_check(probe_image(img("notimage.jpg"))) == DecodeStatus::Broken);
    // the frame types stb_image does not read: arithmetic (SOF9), lossless (SOF3), 12-bit
    {
        const std::string j = img("graph.jpg");
        const std::size_t i = j.find("\xFF\xC0");
        CHECK(i != std::string::npos);
        std::string a9 = j, l3 = j, b12 = j;
        a9[i + 1] = static_cast<char>(0xC9);
        l3[i + 1] = static_cast<char>(0xC3);
        b12[i + 4] = 12;
        CHECK(decode_check(probe_image(a9)) == DecodeStatus::Unsupported);
        CHECK(decode_check(probe_image(l3)) == DecodeStatus::Unsupported);
        CHECK(decode_check(probe_image(b12)) == DecodeStatus::Unsupported);
        DecodedImage d;
        CHECK(decode_image(a9, 100, 100, 0, d) == DecodeStatus::Unsupported);
    }
    // the pixel limits and the dimension limit, from the headers alone
    {
        ImageProbe p = probe_image(img("graph.jpg"));
        p.width = 4000, p.height = 3000;                                  // 12 MP baseline: fine
        CHECK(decode_check(p) == DecodeStatus::Ok);
        p.width = 5000, p.height = 3200;                                  // 16 MP: at the limit
        CHECK(decode_check(p) == DecodeStatus::Ok);
        p.width = 5000, p.height = 3300;
        CHECK(decode_check(p) == DecodeStatus::TooLarge);
        p.progressive = true, p.baseline = false, p.jpeg_sof = 0xC2;
        p.width = 3000, p.height = 2000;                                  // 6 MP progressive: at the limit
        CHECK(decode_check(p) == DecodeStatus::Ok);
        p.width = 3264, p.height = 2448;                                  // 8 MP
        CHECK(decode_check(p) == DecodeStatus::TooLarge);
        p = probe_image(img("graph.jpg"));
        p.width = 20000, p.height = 100;                                  // over the library's side limit, few pixels
        CHECK(decode_check(p) == DecodeStatus::TooLarge);
        p = probe_image(img("logo.png"));
        p.width = 2048, p.height = 2050;                                  // 4.19 MP: under the limit
        CHECK(decode_check(p) == DecodeStatus::Ok);
        p.height = 2100;
        CHECK(decode_check(p) == DecodeStatus::TooLarge);
    }
    CHECK_EQ(rss_image_max_dimension(), 16384u);
}

/* The memory ceiling: the same picture decodes with room and is refused (TooLarge, nothing kept) when the allocator is
 * limited, and the decoder's own peak count stays under what it was given. */
void test_ceiling()
{
    const std::string bytes = img("progressive.jpg");
    unsigned w = 0, h = 0;
    CHECK(rss_image_info(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), &w, &h) && w == 480 && h == 320);
    struct S {
        static int begin(void *, unsigned, unsigned) { return 1; }
        static int rows(void *, unsigned, unsigned, unsigned, unsigned, const unsigned char *, int, std::size_t) { return 1; }
    };
    const rss_img_sink sink{nullptr, S::begin, S::rows};
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    CHECK_EQ(rss_image_decode(data, bytes.size(), 3, 64u * 1024 * 1024, &sink), RSS_IMG_OK);
    const std::size_t peak = rss_image_last_peak();
    CHECK(peak > 480u * 320u * 3u && peak < 8u * 1024 * 1024);          // a few bytes a pixel
    CHECK_EQ(rss_image_decode(data, bytes.size(), 3, peak / 2, &sink), RSS_IMG_TOO_LARGE);
    CHECK(rss_image_last_peak() <= peak / 2);
    CHECK_EQ(rss_image_decode(data, bytes.size(), 3, peak, &sink), RSS_IMG_OK);        // exactly enough
    CHECK_EQ(rss_image_decode(data, bytes.size(), 5, 0, &sink), RSS_IMG_BROKEN);        // only 3 or 4 bytes a pixel
}

void test_cancel()
{
    std::atomic<bool> stop{true};
    DecodedImage d;
    CHECK(decode_image(img("wide.jpg"), 300, 300, 0, d, &stop) == DecodeStatus::Cancelled && d.rgb565.empty());
    CHECK(decode_image(img("logo.png"), 300, 300, 0, d, &stop) == DecodeStatus::Cancelled);
    CHECK(decode_image(img("progressive.jpg"), 300, 300, 0, d, &stop) == DecodeStatus::Cancelled);
}

void test_turns()
{
    const std::string a = img("wide.jpg"), b = img("logo.png"), c = img("anim.gif"), e = img("progressive.jpg");
    std::atomic<int> ok{0};
    std::vector<std::thread> t;
    for (int i = 0; i < 8; ++i)
        t.emplace_back([&, i] {
            DecodedImage d;
            for (int k = 0; k < 4; ++k)
                if (decode_image(i % 4 == 0 ? a : i % 4 == 1 ? b : i % 4 == 2 ? c : e, 320, 200, 0, d) == DecodeStatus::Ok) ++ok;
        });
    for (std::thread &x : t) x.join();
    CHECK_EQ(ok.load(), 32);
    CHECK_EQ(decodes_peak_parallel(), 1);             // they took turns
}

/* Mutations of one fixture of every format: flipped bytes (some in the first 64, where the header fields are), cut ends, a
 * header that lies about the size. Never a crash (run under ASan + UBSan: build.sh asan / tests/flows). */
void test_fuzz()
{
    const char *env = std::getenv("RSS_FUZZ_N");
    const int per = env && std::atoi(env) > 0 ? std::atoi(env) : 600;
    std::mt19937 rnd(1234);
    int runs = 0, ok = 0, broken = 0, other = 0;
    for (const char *name : {"graph.jpg", "progressive.jpg", "baseline444.jpg", "logo.png", "anim.gif", "still.gif", "pic.bmp"}) {
        const std::string good = img(name);
        for (int k = 0; k < per; ++k) {
            std::string m = good;
            const int flips = 1 + static_cast<int>(rnd() % 12);
            for (int f = 0; f < flips; ++f) {
                const std::size_t at = k % 3 == 0 ? rnd() % std::min<std::size_t>(64, m.size()) : rnd() % m.size();
                m[at] = static_cast<char>(rnd() & 0xFF);
            }
            if (k % 5 == 0) m.resize(rnd() % m.size());
            DecodedImage d;
            const DecodeStatus st = decode_image(m, 200, 200, 0, d);
            if (st == DecodeStatus::Ok) {
                CHECK(d.rgb565.size() == static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height) * 2u);
                ++ok;
            } else if (st == DecodeStatus::Broken) {
                ++broken;
            } else {
                ++other;
            }
            ++runs;
        }
    }
    std::printf("fuzz: %d mutated files (7 fixtures x %d): %d decoded, %d broken, %d refused (size or memory)\n", runs, per, ok, broken, other);
    CHECK(runs == 7 * per);
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
    std::printf("%s %s%s %dx%d (%.1f MP, %zu bytes) -> %dx%d: %s in %.0f ms; RSS before %ld KB, peak %ld KB (+%ld KB), decoder allocs %zu KB, result %zu KB\n",
                path.c_str(), image_format_name(p.format), p.progressive ? " progressive" : "", p.width, p.height, static_cast<double>(p.pixels()) / 1e6,
                bytes.size(), d.width, d.height, decode_status_name(st), ms, base, status_kb("VmHWM:"), status_kb("VmHWM:") - base,
                rss_image_last_peak() / 1024, d.rgb565.size() / 1024);
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
    if (argc >= 5 && std::string(argv[1]) == "--bench") return bench(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    if (argc >= 5 && std::string(argv[1]) == "--article") return article(std::atoi(argv[2]), std::atoi(argv[3]), std::vector<std::string>(argv + 4, argv + argc));
    test_fit();
    test_decode_fixtures();
    test_check();
    test_ceiling();
    test_cancel();
    test_turns();
    test_fuzz();
    std::printf("imgtest: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
