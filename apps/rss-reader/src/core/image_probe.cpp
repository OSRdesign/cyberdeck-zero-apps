/*
 * SPDX-License-Identifier: MIT
 */
#include "image_probe.hpp"

#include <cstdlib>
#include <cstring>

namespace rssreader {

namespace {

using u8 = unsigned char;

inline unsigned at(const std::string &b, std::size_t i) { return static_cast<u8>(b[i]); }
inline unsigned be16(const std::string &b, std::size_t i) { return at(b, i) << 8 | at(b, i + 1); }
inline unsigned le16(const std::string &b, std::size_t i) { return at(b, i) | at(b, i + 1) << 8; }
inline std::uint32_t be32(const std::string &b, std::size_t i) { return static_cast<std::uint32_t>(be16(b, i)) << 16 | be16(b, i + 2); }
inline std::uint32_t le32(const std::string &b, std::size_t i) { return le16(b, i) | static_cast<std::uint32_t>(le16(b, i + 2)) << 16; }
inline bool has(const std::string &b, std::size_t i, const char *s) { return b.size() >= i + std::strlen(s) && b.compare(i, std::strlen(s), s) == 0; }

int clamp_dim(std::uint64_t v) { return v > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(v); }

void probe_png(const std::string &b, ImageProbe &p)
{
    p.format = ImageFormat::Png;
    if (b.size() < 29 || !has(b, 12, "IHDR")) return;
    p.width = clamp_dim(be32(b, 16));
    p.height = clamp_dim(be32(b, 20));
    const unsigned colour = at(b, 25);
    p.alpha = colour == 4 || colour == 6;
    p.interlaced = at(b, 28) == 1;
}

/* The markers up to the frame header (SOFn): its type, precision, size and components. Stops at the scan (SOS), the end
 * (EOI), a byte that is not a marker or the end of the bytes. */
void probe_jpeg(const std::string &b, ImageProbe &p)
{
    p.format = ImageFormat::Jpeg;
    std::size_t pos = 2;
    while (pos < b.size()) {
        if (at(b, pos) != 0xFF) return;                 // not a marker where one must be: broken
        while (pos < b.size() && at(b, pos) == 0xFF) ++pos;   // fill bytes
        if (pos >= b.size()) return;
        const unsigned m = at(b, pos++);
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   // no length
        if (m == 0xD9 || m == 0xDA) return;             // the end, or the scan before any frame header
        if (pos + 2 > b.size()) return;
        const unsigned len = be16(b, pos);
        if (len < 2) return;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            p.jpeg_sof = static_cast<int>(m);
            p.progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
            if (pos + 8 > b.size() || len < 8) return;
            const unsigned precision = at(b, pos + 2);
            p.height = static_cast<int>(be16(b, pos + 3));
            p.width = static_cast<int>(be16(b, pos + 5));
            p.components = static_cast<int>(at(b, pos + 7));
            p.precision = static_cast<int>(precision);
            p.baseline = m == 0xC0 && precision == 8;
            return;
        }
        pos += len;
    }
}

void probe_webp(const std::string &b, ImageProbe &p)
{
    p.format = ImageFormat::Webp;
    if (b.size() < 30) return;
    if (has(b, 12, "VP8 ")) {
        if (at(b, 23) == 0x9D && at(b, 24) == 0x01 && at(b, 25) == 0x2A) {
            p.width = static_cast<int>(le16(b, 26) & 0x3FFF);
            p.height = static_cast<int>(le16(b, 28) & 0x3FFF);
        }
    } else if (has(b, 12, "VP8L")) {
        if (at(b, 20) == 0x2F) {
            const std::uint32_t bits = le32(b, 21);
            p.width = static_cast<int>((bits & 0x3FFF) + 1);
            p.height = static_cast<int>(((bits >> 14) & 0x3FFF) + 1);
        }
    } else if (has(b, 12, "VP8X")) {
        p.width = static_cast<int>(1 + (at(b, 24) | at(b, 25) << 8 | at(b, 26) << 16));
        p.height = static_cast<int>(1 + (at(b, 27) | at(b, 28) << 8 | at(b, 29) << 16));
    }
}

bool looks_like_svg(const std::string &b)
{
    const std::string head = b.substr(0, 1024);
    std::size_t i = 0;
    if (has(head, 0, "\xEF\xBB\xBF")) i = 3;
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')) ++i;
    if (has(head, i, "<svg")) return true;
    return (has(head, i, "<?xml") || has(head, i, "<!DOCTYPE svg")) && head.find("<svg") != std::string::npos;
}

} // namespace

ImageProbe probe_image(const std::string &b)
{
    ImageProbe p;
    if (b.size() >= 8 && has(b, 0, "\x89PNG\r\n\x1A\n")) {
        probe_png(b, p);
    } else if (b.size() >= 3 && at(b, 0) == 0xFF && at(b, 1) == 0xD8 && at(b, 2) == 0xFF) {
        probe_jpeg(b, p);
    } else if (b.size() >= 6 && (has(b, 0, "GIF87a") || has(b, 0, "GIF89a"))) {
        p.format = ImageFormat::Gif;
        if (b.size() >= 10) {
            p.width = static_cast<int>(le16(b, 6));
            p.height = static_cast<int>(le16(b, 8));
        }
    } else if (b.size() >= 16 && has(b, 0, "RIFF") && has(b, 8, "WEBP")) {
        probe_webp(b, p);
    } else if (b.size() >= 2 && has(b, 0, "BM")) {
        p.format = ImageFormat::Bmp;
        if (b.size() >= 26) {
            p.width = clamp_dim(static_cast<std::uint64_t>(std::abs(static_cast<long long>(static_cast<std::int32_t>(le32(b, 18))))));
            p.height = clamp_dim(static_cast<std::uint64_t>(std::abs(static_cast<long long>(static_cast<std::int32_t>(le32(b, 22))))));
        }
    } else if (b.size() >= 12 && has(b, 4, "ftyp") && (has(b, 8, "avif") || has(b, 8, "avis"))) {
        p.format = ImageFormat::Avif;
    } else if (looks_like_svg(b)) {
        p.format = ImageFormat::Svg;
    }
    return p;
}

const char *image_format_name(ImageFormat f)
{
    switch (f) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Webp: return "WebP";
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Svg: return "SVG";
    case ImageFormat::Avif: return "AVIF";
    case ImageFormat::Unknown: break;
    }
    return "unknown";
}

} // namespace rssreader
