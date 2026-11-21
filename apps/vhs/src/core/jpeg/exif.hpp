/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the photo file (brief section 7). The file is the camera's own JPEG frame with an EXIF APP1 segment right after
 * SOI, an APP0 (JFIF) segment and any EXIF APP1 the frame carried removed, and the Annex K Huffman tables added only
 * when the frame has none (dht.hpp). Nothing is decoded or re-encoded.
 *
 * The EXIF writer is the app's own (little-endian TIFF, two IFDs); the reader parses back what it writes (and the same
 * tags of a big-endian file) for the tests and the gate's parse-back report. Tags:
 *   IFD0  Make (0x010F, the USB manufacturer string), Model (0x0110, the USB product string), Orientation (0x0112, 1: a
 *         turned camera already sends upright frames), Software (0x0131, "VHS 0.1.2"), DateTime (0x0132, only when the
 *         clock is synchronised), the Exif IFD pointer (0x8769)
 *   Exif  DateTimeOriginal (0x9003, only when the clock is synchronised), PixelXDimension (0xA002), PixelYDimension
 *         (0xA003)
 * An empty Make or Model is left out. Pure, LVGL-free.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

constexpr const char *kSoftware = "VHS 0.1.2";

struct ExifFields {
    std::string make, model, software;
    std::string datetime;          /* "YYYY:MM:DD HH:MM:SS" for DateTime and DateTimeOriginal; "" = the clock is not set */
    std::string datetime_original; /* the reader's: DateTimeOriginal ("" when absent) */
    int orientation = 1;
    std::uint32_t width = 0, height = 0;
    bool has_orientation = false;  /* the reader's */
    bool has_exif_ifd = false;     /* the reader's */
};

/* The whole APP1 segment (FF E1, length, "Exif\0\0", the TIFF block). */
std::vector<std::uint8_t> exif_app1(const ExifFields &f);

/* The EXIF APP1 of a JPEG file, parsed; false when it has none or it cannot be read. */
bool exif_parse(const std::uint8_t *jpeg, std::size_t n, ExifFields &out);

/* "YYYY:MM:DD HH:MM:SS" of a wall-clock second in local time (utc: in UTC); "" when the conversion fails. */
std::string exif_datetime(std::int64_t wall_s, bool utc = false);

struct PhotoFacts {
    bool dht_inserted = false;
    bool app0_removed = false;
    int width = 0, height = 0;
};

/* out = the photo file of a camera frame (the header comment). False with err when the frame is not a JPEG. width and
 * height of f are filled from the frame's SOF when they are 0. */
bool make_photo(const std::uint8_t *frame, std::size_t n, ExifFields f, std::vector<std::uint8_t> &out, std::string *err,
                PhotoFacts *facts = nullptr);

/* The marker of every segment before SOS, in order (tests: "FFD8 FFE1 FFDB FFC4 FFC0 FFDA"). */
std::vector<std::uint8_t> jpeg_markers(const std::uint8_t *jpeg, std::size_t n);

} // namespace vhs
