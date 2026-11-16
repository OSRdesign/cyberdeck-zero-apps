/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: TurboJPEG (libjpeg-turbo's TurboJPEG API 2.x, package libturbojpeg0) opened at run time with dlopen: no
 * link-time dependency, no header needed at build time. The library is looked up once per process: $VHS_TURBOJPEG
 * (a path, for tests) or "libturbojpeg.so.0". Without it, available() is false and error() says how to install it;
 * the YUYV modes still work.
 *
 * Decode is the plan's D5: tjDecompress2 at a scaling factor 1/1, 1/2, 1/4 or 1/8 (the IDCT does the scaling, LVGL never
 * scales), into BGRX (LVGL's XRGB8888 byte order), TJFLAG_FASTDCT unless asked otherwise. A Decoder / Encoder is one
 * TurboJPEG handle: one per thread.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

class TurboJpeg {
public:
    /* The process's loader (thread-safe, loads on the first call). */
    static TurboJpeg &get();

    bool available() const { return ok_; }
    bool can_encode() const { return ok_ && init_compress_ && compress2_ && free_; }
    const std::string &error() const { return error_; }
    const std::string &path() const { return path_; }

    struct Api; /* the resolved functions */
    const Api *api() const { return api_; }

    TurboJpeg(const TurboJpeg &) = delete;
    TurboJpeg &operator=(const TurboJpeg &) = delete;

private:
    TurboJpeg();
    bool ok_ = false;
    bool init_compress_ = false, compress2_ = false, free_ = false;
    std::string error_, path_;
    const Api *api_ = nullptr;
};

/* TJSCALED(dim, 1/denom): the decoded size of dim at 1/denom (rounded up, as libjpeg-turbo does). */
inline int tj_scaled(int dim, int denom)
{
    return denom <= 1 ? dim : (dim + denom - 1) / denom;
}

struct JpegInfo {
    int w = 0, h = 0;
    int subsamp = -1; /* TJSAMP_* (0 444, 1 422, 2 420, 3 gray, 4 440, 5 411) */
};

class JpegDecoder {
public:
    explicit JpegDecoder(TurboJpeg &tj = TurboJpeg::get());
    ~JpegDecoder();
    JpegDecoder(const JpegDecoder &) = delete;
    JpegDecoder &operator=(const JpegDecoder &) = delete;

    bool ok() const { return handle_ != nullptr; }
    bool header(const std::uint8_t *jpeg, std::size_t size, JpegInfo &info);
    /* Decodes at 1/denom (1, 2, 4, 8) into dst (BGRX, pitch bytes per row, at least tj_scaled(w) x tj_scaled(h)).
     * 0: done; 1: done with a warning (a damaged frame still decoded); -1: failed (last_error()). */
    int decode_bgrx(const std::uint8_t *jpeg, std::size_t size, int denom, std::uint8_t *dst, int pitch, bool fastdct);
    std::string last_error() const;

private:
    TurboJpeg &tj_;
    void *handle_ = nullptr;
};

class JpegEncoder {
public:
    explicit JpegEncoder(TurboJpeg &tj = TurboJpeg::get());
    ~JpegEncoder();
    JpegEncoder(const JpegEncoder &) = delete;
    JpegEncoder &operator=(const JpegEncoder &) = delete;

    bool ok() const { return handle_ != nullptr; }
    /* BGRX w x h -> a baseline JPEG with its Huffman tables (4:2:2, as the camera sends), quality 1..100. */
    bool encode_bgrx(const std::uint8_t *bgrx, int w, int h, int quality, std::vector<std::uint8_t> &out);

private:
    TurboJpeg &tj_;
    void *handle_ = nullptr;
};

/* The JPEG markers of a frame (no library): its size from the SOF segment and whether it carries Huffman tables (DHT).
 * False when no SOF0..SOF2 is found before SOS. */
bool jpeg_peek(const std::uint8_t *jpeg, std::size_t size, int &w, int &h, bool &has_dht);

/* Splits concatenated JPEG images (an .mjpg file as `ffmpeg -c copy -f mjpeg` writes it) at their SOI / EOI markers. */
std::vector<std::vector<std::uint8_t>> split_mjpeg(const std::vector<std::uint8_t> &bytes);

} // namespace vhs
