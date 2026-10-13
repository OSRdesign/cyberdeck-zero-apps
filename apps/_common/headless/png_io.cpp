/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness: PNG files (libpng), captions (FreeType) and contact sheets.
 */
#include "headless.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace headless {

bool write_png(const std::string &path, const Image &img)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png) png_destroy_write_struct(&png, info ? &info : nullptr);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, static_cast<png_uint_32>(img.w), static_cast<png_uint_32>(img.h), 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_byte> row(static_cast<size_t>(img.w) * 3);
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const uint32_t p = img.px[static_cast<size_t>(y) * img.w + x];
            row[x * 3] = static_cast<png_byte>(p >> 16);
            row[x * 3 + 1] = static_cast<png_byte>(p >> 8);
            row[x * 3 + 2] = static_cast<png_byte>(p);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return true;
}

bool read_png(const std::string &path, Image &img)
{
    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) return false;
    image.format = PNG_FORMAT_BGRA;
    std::vector<uint32_t> buf(static_cast<size_t>(image.width) * image.height);
    if (!png_image_finish_read(&image, nullptr, buf.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    img.w = static_cast<int>(image.width);
    img.h = static_cast<int>(image.height);
    img.px.resize(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) img.px[i] = buf[i] & 0xFFFFFFu; /* BGRA bytes = 0xAARRGGBB little endian */
    return true;
}

bool draw_text(Image &img, const std::string &font, int px, int x, int y, uint32_t rgb, const std::string &text)
{
    static FT_Library lib = nullptr;
    static FT_Face face = nullptr;
    static std::string loaded;
    if (!lib && FT_Init_FreeType(&lib) != 0) return false;
    if (loaded != font) {
        if (face) FT_Done_Face(face);
        face = nullptr;
        if (FT_New_Face(lib, font.c_str(), 0, &face) != 0) return false;
        loaded = font;
    }
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px));
    const int baseline = y + static_cast<int>(face->size->metrics.ascender >> 6);
    int pen = x;
    for (const unsigned char c : text) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot g = face->glyph;
        for (unsigned row = 0; row < g->bitmap.rows; ++row)
            for (unsigned col = 0; col < g->bitmap.width; ++col) {
                const int dx = pen + g->bitmap_left + static_cast<int>(col);
                const int dy = baseline - g->bitmap_top + static_cast<int>(row);
                if (dx < 0 || dy < 0 || dx >= img.w || dy >= img.h) continue;
                const unsigned a = g->bitmap.buffer[row * static_cast<unsigned>(g->bitmap.pitch) + col];
                uint32_t &d = img.px[static_cast<size_t>(dy) * img.w + dx];
                const auto mix = [a](uint32_t s, uint32_t t) { return (s * a + t * (255 - a)) / 255; };
                d = (mix((rgb >> 16) & 255, (d >> 16) & 255) << 16) | (mix((rgb >> 8) & 255, (d >> 8) & 255) << 8) |
                    mix(rgb & 255, d & 255);
            }
        pen += static_cast<int>(g->advance.x >> 6);
    }
    return true;
}

bool write_sheet(const std::string &path, const std::string &title, const std::vector<std::string> &sizes,
                 const std::vector<std::string> &shots, const std::string &out_dir, const std::string &font)
{
    constexpr int kGap = 24, kFrame = 2, kCaption = 30, kTitle = 44;
    constexpr uint32_t kBack = 0x202020, kFrameRgb = 0x707070, kText = 0xE0E0E0;
    std::vector<int> col_w;
    std::vector<int> row_h(shots.size(), 0);
    std::vector<std::vector<Image>> cells(shots.size(), std::vector<Image>(sizes.size()));
    for (size_t c = 0; c < sizes.size(); ++c) {
        int w = 0;
        for (size_t r = 0; r < shots.size(); ++r) {
            Image &img = cells[r][c];
            if (!read_png(out_dir + "/" + sizes[c] + "/" + shots[r] + ".png", img)) continue;
            w = std::max(w, img.w);
            row_h[r] = std::max(row_h[r], img.h);
        }
        col_w.push_back(w);
    }
    Image sheet;
    sheet.w = kGap;
    for (int w : col_w) sheet.w += w + 2 * kFrame + kGap;
    sheet.h = kTitle;
    for (int h : row_h) sheet.h += kCaption + h + 2 * kFrame + kGap;
    if (sheet.w <= kGap || sheet.h <= kTitle) return false;
    sheet.px.assign(static_cast<size_t>(sheet.w) * sheet.h, kBack);
    draw_text(sheet, font, 26, kGap, 8, kText, title);
    int y = kTitle;
    for (size_t r = 0; r < shots.size(); ++r) {
        int x = kGap;
        for (size_t c = 0; c < sizes.size(); ++c) {
            draw_text(sheet, font, 18, x, y + 4, kText, shots[r] + "  " + sizes[c]);
            const Image &img = cells[r][c];
            if (img.w > 0) {
                const int ox = x + kFrame, oy = y + kCaption + kFrame;
                for (int fy = oy - kFrame; fy < oy + img.h + kFrame; ++fy)
                    for (int fx = ox - kFrame; fx < ox + img.w + kFrame; ++fx)
                        if (fx < ox || fy < oy || fx >= ox + img.w || fy >= oy + img.h)
                            sheet.px[static_cast<size_t>(fy) * sheet.w + fx] = kFrameRgb;
                for (int iy = 0; iy < img.h; ++iy)
                    std::copy_n(&img.px[static_cast<size_t>(iy) * img.w], img.w,
                                &sheet.px[static_cast<size_t>(oy + iy) * sheet.w + ox]);
            }
            x += col_w[c] + 2 * kFrame + kGap;
        }
        y += kCaption + row_h[r] + 2 * kFrame + kGap;
    }
    return write_png(path, sheet);
}

} // namespace headless
