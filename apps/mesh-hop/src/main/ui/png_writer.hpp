/*
 * SPDX-License-Identifier: MIT
 *
 * A tiny PNG writer (stored deflate blocks, no compression) for the screenshots of the headless mode.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace meshhop {

/* xrgb: width*height pixels of 0x00RRGGBB (row after row, `stride_px` pixels per row). The encoded PNG file. */
std::vector<uint8_t> encode_png(const uint32_t *xrgb, int width, int height, int stride_px);
bool write_png(const std::string &path, const uint32_t *xrgb, int width, int height, int stride_px);

uint32_t crc32_bytes(const uint8_t *data, size_t len, uint32_t crc = 0);
uint32_t adler32_bytes(const uint8_t *data, size_t len);

} // namespace meshhop
