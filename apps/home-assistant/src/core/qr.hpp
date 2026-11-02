/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the pairing QR code as a matrix of modules (no LVGL; tests/test_qr.cpp). The encoder is
 * Project Nayuki's QR Code generator library (qrcodegen.cpp / .hpp, v1.8.0, MIT, unchanged), low error correction
 * raised for free when it fits the same version (the probe's choice: about 200 characters give version 9, 53 modules).
 */
#pragma once

#include <string>
#include <vector>

namespace ha {

struct QrMatrix {
    int size = 0;                /* modules per side, 0 = could not encode */
    std::vector<unsigned char> dark; /* size * size, row by row, 1 = dark module */
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[y * size + x]; }
};

QrMatrix qr_encode(const std::string &text);

/* The pixel size of one module and the side of the drawn code (modules + a quiet zone of `quiet` modules on each side)
 * for a box of `box` pixels: the largest whole number of pixels per module that fits, at least 1. */
int qr_module_px(int modules, int quiet, int box);

} // namespace ha
