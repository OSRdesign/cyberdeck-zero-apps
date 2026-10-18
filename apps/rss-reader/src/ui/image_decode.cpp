/*
 * SPDX-License-Identifier: MIT
 */
#include "image_decode.hpp"

#include "image_codecs.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif

using rssreader::ImageFormat;
using rssreader::ImageProbe;

namespace rssui {

namespace {

std::mutex g_turn;                 // one decode at a time in the whole process
std::atomic<int> g_done{0}, g_running{0}, g_peak{0};

/* The area average of a picture into a smaller one. Every target pixel starts as the background colour times the number
 * of source pixels it covers; each source pixel that arrives replaces one background share by its own value (modulo
 * 2^32 arithmetic, exact in the end): the parts of the picture that never arrive (a transparent pixel, a frame smaller than
 * its canvas) stay background. 12 bytes per target pixel (and 4 per source row and column), freed with the resampler. */
class Resampler {
public:
    bool init(int sw, int sh, int tw, int th, std::uint32_t bg)
    {
        sw_ = sw;
        sh_ = sh;
        tw_ = tw;
        th_ = th;
        bg_[0] = (bg >> 16) & 0xFF;
        bg_[1] = (bg >> 8) & 0xFF;
        bg_[2] = bg & 0xFF;
        try {
            mapx_.resize(static_cast<std::size_t>(sw));
            mapy_.resize(static_cast<std::size_t>(sh));
            nx_.assign(static_cast<std::size_t>(tw), 0);
            ny_.assign(static_cast<std::size_t>(th), 0);
            auto &nx = nx_;
            auto &ny = ny_;
            for (int x = 0; x < sw; ++x) {
                mapx_[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<std::int64_t>(x) * tw / sw);
                ++nx[static_cast<std::size_t>(mapx_[static_cast<std::size_t>(x)])];
            }
            for (int y = 0; y < sh; ++y) {
                mapy_[static_cast<std::size_t>(y)] = static_cast<int>(static_cast<std::int64_t>(y) * th / sh);
                ++ny[static_cast<std::size_t>(mapy_[static_cast<std::size_t>(y)])];
            }
            acc_.resize(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 3);
            std::size_t i = 0;
            for (int ty = 0; ty < th; ++ty)
                for (int tx = 0; tx < tw; ++tx, ++i) {
                    const std::uint32_t n = nx[static_cast<std::size_t>(tx)] * ny[static_cast<std::size_t>(ty)];
                    for (int c = 0; c < 3; ++c) acc_[i * 3 + static_cast<std::size_t>(c)] = n * bg_[c];
                }
        } catch (const std::bad_alloc &) {
            return false;
        }
        return true;
    }
    /* n pixels of source row y from column x0, 3 bytes each (R, G, B) */
    void rgb(int y, int x0, const std::uint8_t *p, int n)
    {
        if (y < 0 || y >= sh_ || x0 >= sw_) return;
        if (x0 < 0) {
            p += static_cast<std::ptrdiff_t>(-x0) * 3;
            n += x0;
            x0 = 0;
        }
        n = std::min(n, sw_ - x0);
        std::uint32_t *line = &acc_[static_cast<std::size_t>(mapy_[static_cast<std::size_t>(y)]) * static_cast<std::size_t>(tw_) * 3];
        for (int i = 0; i < n; ++i, p += 3) {
            std::uint32_t *a = line + static_cast<std::size_t>(mapx_[static_cast<std::size_t>(x0 + i)]) * 3;
            a[0] += static_cast<std::uint32_t>(p[0]) - bg_[0];
            a[1] += static_cast<std::uint32_t>(p[1]) - bg_[1];
            a[2] += static_cast<std::uint32_t>(p[2]) - bg_[2];
        }
    }
    /* the same with alpha (R, G, B, A, straight): blended over the background */
    void rgba(int y, int x0, const std::uint8_t *p, int n)
    {
        if (y < 0 || y >= sh_ || x0 >= sw_ || x0 < 0) return;
        n = std::min(n, sw_ - x0);
        std::uint32_t *line = &acc_[static_cast<std::size_t>(mapy_[static_cast<std::size_t>(y)]) * static_cast<std::size_t>(tw_) * 3];
        for (int i = 0; i < n; ++i, p += 4) {
            const std::uint32_t al = p[3];
            if (al == 0) continue;
            std::uint32_t *a = line + static_cast<std::size_t>(mapx_[static_cast<std::size_t>(x0 + i)]) * 3;
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t v = al == 255 ? p[c] : (p[c] * al + bg_[c] * (255 - al) + 127) / 255;
                a[c] += v - bg_[c];
            }
        }
    }
    void finish(std::vector<std::uint8_t> &out)
    {
        out.assign(static_cast<std::size_t>(tw_) * static_cast<std::size_t>(th_) * 2, 0);
        const std::size_t count = static_cast<std::size_t>(tw_) * static_cast<std::size_t>(th_);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t n = std::max<std::uint32_t>(1, nx_[i % static_cast<std::size_t>(tw_)] * ny_[i / static_cast<std::size_t>(tw_)]);
            const std::uint32_t r = (acc_[i * 3] + n / 2) / n, g = (acc_[i * 3 + 1] + n / 2) / n, b = (acc_[i * 3 + 2] + n / 2) / n;
            const std::uint16_t v = static_cast<std::uint16_t>((std::min<std::uint32_t>(r, 255) >> 3) << 11 | (std::min<std::uint32_t>(g, 255) >> 2) << 5 |
                                                               (std::min<std::uint32_t>(b, 255) >> 3));
            out[i * 2] = static_cast<std::uint8_t>(v & 0xFF);
            out[i * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        std::vector<std::uint32_t>().swap(acc_);
    }

private:
    int sw_ = 0, sh_ = 0, tw_ = 0, th_ = 0;
    std::uint32_t bg_[3] = {0, 0, 0};
    std::vector<int> mapx_, mapy_;
    std::vector<std::uint32_t> acc_, nx_, ny_;
};

struct Job {
    int max_w = 1, max_h = 1;
    std::uint64_t max_pixels = 0;
    std::uint32_t bg = 0;
    const std::atomic<bool> *cancel = nullptr;
    DecodedImage *out = nullptr;
    Resampler rs;
    bool ready = false, too_large = false;
    bool cancelled() const { return cancel && cancel->load(); }
};

int sink_begin(void *user, unsigned sw, unsigned sh)
{
    Job *j = static_cast<Job *>(user);
    if (j->cancelled()) return 0;
    if (sw == 0 || sh == 0 || static_cast<std::uint64_t>(sw) * sh > j->max_pixels) {
        j->too_large = true;
        return 0;
    }
    int tw = 1, th = 1;
    fit_size(static_cast<int>(sw), static_cast<int>(sh), j->max_w, j->max_h, tw, th);
    j->out->src_width = static_cast<int>(sw);
    j->out->src_height = static_cast<int>(sh);
    j->out->width = tw;
    j->out->height = th;
    j->ready = j->rs.init(static_cast<int>(sw), static_cast<int>(sh), tw, th, j->bg);
    if (!j->ready) j->too_large = true;           // no memory for the result
    return j->ready ? 1 : 0;
}

int sink_rows(void *user, unsigned left, unsigned top, unsigned right, unsigned bottom, const unsigned char *px, int bpp, std::size_t stride)
{
    Job *j = static_cast<Job *>(user);
    if (!j->ready || j->cancelled()) return 0;
    const int n = static_cast<int>(right - left + 1);
    for (unsigned y = top; y <= bottom; ++y, px += stride) {
        if (bpp == 4) j->rs.rgba(static_cast<int>(y), static_cast<int>(left), px, n);
        else j->rs.rgb(static_cast<int>(y), static_cast<int>(left), px, n);
    }
    return 1;
}

std::uint64_t pixel_limit(ImageFormat f)
{
    switch (f) {
    case ImageFormat::Jpeg: return kMaxJpegPixels;
    case ImageFormat::Png: return kMaxPngPixels;
    case ImageFormat::Gif: return kMaxGifPixels;
    default: return 0;
    }
}

} // namespace

const char *decode_status_name(DecodeStatus s)
{
    switch (s) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::Unsupported: return "unsupported";
    case DecodeStatus::TooLarge: return "too large";
    case DecodeStatus::Broken: return "broken";
    case DecodeStatus::Cancelled: return "cancelled";
    }
    return "?";
}

void fit_size(int sw, int sh, int max_w, int max_h, int &w, int &h)
{
    sw = std::max(1, sw);
    sh = std::max(1, sh);
    max_w = std::max(1, max_w);
    max_h = std::max(1, max_h);
    w = sw;
    h = sh;
    if (w > max_w) {
        w = max_w;
        h = static_cast<int>((static_cast<std::int64_t>(sh) * max_w + sw / 2) / sw);
    }
    if (h > max_h) {
        h = max_h;
        w = static_cast<int>((static_cast<std::int64_t>(sw) * max_h + sh / 2) / sh);
    }
    w = std::clamp(w, 1, std::min(sw, max_w));
    h = std::clamp(h, 1, std::min(sh, max_h));
}

DecodeStatus decode_check(const ImageProbe &p)
{
    if (p.format == ImageFormat::Unknown) return DecodeStatus::Broken;
    switch (p.format) {
    case ImageFormat::Jpeg:
        if (!rss_jpeg_available() || (p.jpeg_sof >= 0 && !p.baseline) || p.components == 4) return DecodeStatus::Unsupported;
        break;
    case ImageFormat::Png:
        if (!rss_png_available()) return DecodeStatus::Unsupported;
        break;
    case ImageFormat::Gif:
        if (!rss_gif_available() || p.width > rss_gif_max_width()) return DecodeStatus::Unsupported;
        break;
    default: return DecodeStatus::Unsupported;     // WebP, BMP, SVG, AVIF: no decoder in the pin
    }
    if (p.width <= 0 || p.height <= 0) return DecodeStatus::Broken;
    return p.pixels() > pixel_limit(p.format) ? DecodeStatus::TooLarge : DecodeStatus::Ok;
}

DecodeStatus decode_image(const std::string &bytes, int max_w, int max_h, std::uint32_t bg, DecodedImage &out, const std::atomic<bool> *cancel)
{
    out = DecodedImage();
    const ImageProbe p = rssreader::probe_image(bytes);
    const DecodeStatus pre = decode_check(p);
    if (pre != DecodeStatus::Ok) return pre;
    // the turn: wait for the decode before this one to end (or for the cancel)
    std::unique_lock<std::mutex> turn(g_turn, std::defer_lock);
    while (!turn.try_lock()) {
        if (cancel && cancel->load()) return DecodeStatus::Cancelled;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int now = ++g_running;
    int peak = g_peak.load();
    while (now > peak && !g_peak.compare_exchange_weak(peak, now)) {}
    Job j;
    j.max_w = max_w;
    j.max_h = max_h;
    j.max_pixels = pixel_limit(p.format);
    j.bg = bg;
    j.cancel = cancel;
    j.out = &out;
    const rss_img_sink sink{&j, sink_begin, sink_rows};
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    int rc = RSS_IMG_BROKEN;
    switch (p.format) {
    case ImageFormat::Jpeg: rc = rss_jpeg_decode(data, bytes.size(), &sink); break;
    case ImageFormat::Png: rc = rss_png_decode(data, bytes.size(), &sink); break;
    case ImageFormat::Gif: rc = rss_gif_decode(data, bytes.size(), &sink); break;
    default: break;
    }
    DecodeStatus st = DecodeStatus::Broken;
    if (j.cancelled()) st = DecodeStatus::Cancelled;
    else if (rc == RSS_IMG_OK) st = j.ready ? DecodeStatus::Ok : DecodeStatus::Broken;
    else if (rc == RSS_IMG_STOPPED) st = j.too_large ? DecodeStatus::TooLarge : DecodeStatus::Broken;
    else if (rc == RSS_IMG_UNSUPPORTED || rc == RSS_IMG_NO_DECODER) st = DecodeStatus::Unsupported;
    if (st == DecodeStatus::Ok) {
        try {
            j.rs.finish(out.rgb565);
        } catch (const std::bad_alloc &) {
            st = DecodeStatus::TooLarge;
        }
    }
    if (st != DecodeStatus::Ok) out = DecodedImage();
    j.rs = Resampler();
#ifdef __GLIBC__
    // the decoder's big blocks went back to malloc; give them back to the system too (glibc keeps freed heap otherwise:
    // measured, 20 MB stayed after an article with two 4 MP PNGs)
    malloc_trim(0);
#endif
    --g_running;
    ++g_done;
    return st;
}

int decodes_done() { return g_done.load(); }
int decodes_peak_parallel() { return g_peak.load(); }

} // namespace rssui
