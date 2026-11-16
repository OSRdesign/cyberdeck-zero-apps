/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: TurboJPEG through dlopen (see turbojpeg.hpp). The prototypes and constants below are those of the
 * TurboJPEG API 2.x (turbojpeg.h of libjpeg-turbo 2.1, BSD-3-Clause / IJG / zlib), declared here so no header is needed.
 */
#include "turbojpeg.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vhs {
namespace {

using tjhandle = void *;
constexpr int TJPF_BGRX = 3;
constexpr int TJSAMP_422 = 1;
constexpr int TJFLAG_FASTDCT = 2048;
constexpr int TJERR_WARNING = 0;

} // namespace

struct TurboJpeg::Api {
    tjhandle (*init_decompress)() = nullptr;
    int (*decompress_header3)(tjhandle, const unsigned char *, unsigned long, int *, int *, int *, int *) = nullptr;
    int (*decompress2)(tjhandle, const unsigned char *, unsigned long, unsigned char *, int, int, int, int, int) = nullptr;
    int (*destroy)(tjhandle) = nullptr;
    char *(*get_error_str2)(tjhandle) = nullptr; /* 2.0+ */
    char *(*get_error_str)() = nullptr;          /* 1.x fallback */
    int (*get_error_code)(tjhandle) = nullptr;   /* 2.0+ */
    tjhandle (*init_compress)() = nullptr;
    int (*compress2)(tjhandle, const unsigned char *, int, int, int, int, unsigned char **, unsigned long *, int, int,
                     int) = nullptr;
    void (*free_buf)(unsigned char *) = nullptr;
};

namespace {

template <typename F> bool sym(void *lib, const char *name, F &out)
{
    void *p = dlsym(lib, name);
    if (!p) return false;
    std::memcpy(&out, &p, sizeof out);
    return true;
}

} // namespace

TurboJpeg::TurboJpeg()
{
    const char *env = std::getenv("VHS_TURBOJPEG");
    path_ = env && env[0] ? env : "libturbojpeg.so.0";
    void *lib = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        error_ = "libturbojpeg0 is not installed (sudo apt install libturbojpeg0)";
        return;
    }
    static Api api;
    bool ok = sym(lib, "tjInitDecompress", api.init_decompress) &&
              sym(lib, "tjDecompressHeader3", api.decompress_header3) && sym(lib, "tjDecompress2", api.decompress2) &&
              sym(lib, "tjDestroy", api.destroy);
    sym(lib, "tjGetErrorStr2", api.get_error_str2);
    sym(lib, "tjGetErrorStr", api.get_error_str);
    sym(lib, "tjGetErrorCode", api.get_error_code);
    init_compress_ = sym(lib, "tjInitCompress", api.init_compress);
    compress2_ = sym(lib, "tjCompress2", api.compress2);
    free_ = sym(lib, "tjFree", api.free_buf);
    if (!ok) {
        error_ = path_ + " lacks the TurboJPEG 1.4+ functions";
        return; /* the library stays loaded: harmless */
    }
    api_ = &api;
    ok_ = true;
}

TurboJpeg &TurboJpeg::get()
{
    static TurboJpeg *tj = nullptr;
    static std::once_flag once;
    std::call_once(once, [] { tj = new TurboJpeg(); }); /* never freed: dlopen'd for the process's life */
    return *tj;
}

JpegDecoder::JpegDecoder(TurboJpeg &tj) : tj_(tj)
{
    if (tj_.available()) handle_ = tj_.api()->init_decompress();
}

JpegDecoder::~JpegDecoder()
{
    if (handle_) tj_.api()->destroy(handle_);
}

bool JpegDecoder::header(const std::uint8_t *jpeg, std::size_t size, JpegInfo &info)
{
    if (!handle_ || !jpeg || size < 4) return false;
    int w = 0, h = 0, ss = -1, cs = -1;
    if (tj_.api()->decompress_header3(handle_, jpeg, static_cast<unsigned long>(size), &w, &h, &ss, &cs) != 0) return false;
    info.w = w;
    info.h = h;
    info.subsamp = ss;
    return w > 0 && h > 0;
}

int JpegDecoder::decode_bgrx(const std::uint8_t *jpeg, std::size_t size, int denom, std::uint8_t *dst, int pitch,
                             bool fastdct)
{
    if (!handle_ || !jpeg || !dst) return -1;
    JpegInfo info;
    if (!header(jpeg, size, info)) return -1;
    const int w = tj_scaled(info.w, denom), h = tj_scaled(info.h, denom);
    const int r = tj_.api()->decompress2(handle_, jpeg, static_cast<unsigned long>(size), dst, w, pitch, h, TJPF_BGRX,
                                         fastdct ? TJFLAG_FASTDCT : 0);
    if (r == 0) return 0;
    if (tj_.api()->get_error_code && tj_.api()->get_error_code(handle_) == TJERR_WARNING) return 1;
    return -1;
}

std::string JpegDecoder::last_error() const
{
    if (!handle_) return tj_.error();
    const char *s = nullptr;
    if (tj_.api()->get_error_str2) s = tj_.api()->get_error_str2(handle_);
    else if (tj_.api()->get_error_str) s = tj_.api()->get_error_str();
    return s ? s : "unknown TurboJPEG error";
}

JpegEncoder::JpegEncoder(TurboJpeg &tj) : tj_(tj)
{
    if (tj_.can_encode()) handle_ = tj_.api()->init_compress();
}

JpegEncoder::~JpegEncoder()
{
    if (handle_) tj_.api()->destroy(handle_);
}

bool JpegEncoder::encode_bgrx(const std::uint8_t *bgrx, int w, int h, int quality, std::vector<std::uint8_t> &out)
{
    if (!handle_ || !bgrx || w <= 0 || h <= 0) return false;
    unsigned char *buf = nullptr;
    unsigned long size = 0;
    const int r = tj_.api()->compress2(handle_, bgrx, w, w * 4, h, TJPF_BGRX, &buf, &size, TJSAMP_422, quality, 0);
    if (r != 0 || !buf) {
        if (buf) tj_.api()->free_buf(buf);
        return false;
    }
    out.assign(buf, buf + size);
    tj_.api()->free_buf(buf);
    return true;
}

bool jpeg_peek(const std::uint8_t *p, std::size_t n, int &w, int &h, bool &has_dht)
{
    w = h = 0;
    has_dht = false;
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
    std::size_t i = 2;
    bool sof = false;
    while (i + 4 <= n) {
        if (p[i] != 0xFF) return false;
        const std::uint8_t m = p[i + 1];
        if (m == 0xFF) { /* fill byte */
            ++i;
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { /* no length */
            i += 2;
            continue;
        }
        const std::size_t len = (static_cast<std::size_t>(p[i + 2]) << 8) | p[i + 3];
        if (len < 2 || i + 2 + len > n) return false;
        if (m == 0xC4) has_dht = true;
        if ((m == 0xC0 || m == 0xC1 || m == 0xC2) && len >= 7) {
            h = (p[i + 5] << 8) | p[i + 6];
            w = (p[i + 7] << 8) | p[i + 8];
            sof = true;
        }
        if (m == 0xDA) break; /* SOS: the tables come before it */
        i += 2 + len;
    }
    return sof && w > 0 && h > 0;
}

std::vector<std::vector<std::uint8_t>> split_mjpeg(const std::vector<std::uint8_t> &b)
{
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t i = 0;
    const std::size_t n = b.size();
    while (i + 1 < n) {
        if (!(b[i] == 0xFF && b[i + 1] == 0xD8)) {
            ++i;
            continue;
        }
        /* walk the segments to SOS, then scan the entropy-coded data for EOI (0xFF 0xD9; 0xFF 0x00 is stuffing) */
        std::size_t j = i + 2;
        bool in_scan = false;
        std::size_t end = 0;
        while (j + 1 < n) {
            if (!in_scan) {
                if (b[j] != 0xFF) break;
                const std::uint8_t m = b[j + 1];
                if (m == 0xFF) {
                    ++j;
                    continue;
                }
                if (m == 0xD9) {
                    end = j + 2;
                    break;
                }
                if (j + 3 >= n) break;
                const std::size_t len = (static_cast<std::size_t>(b[j + 2]) << 8) | b[j + 3];
                if (len < 2) break;
                j += 2 + len;
                if (m == 0xDA) in_scan = true;
                continue;
            }
            if (b[j] == 0xFF && b[j + 1] == 0xD9) {
                end = j + 2;
                break;
            }
            ++j;
        }
        if (!end) break;
        out.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(end));
        i = end;
    }
    return out;
}

} // namespace vhs
