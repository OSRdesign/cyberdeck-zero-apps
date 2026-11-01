/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the pairing QR code (see qr.hpp).
 */
#include "qr.hpp"

#include "qrcodegen.hpp"

#include <exception>

namespace ha {

QrMatrix qr_encode(const std::string &text)
{
    QrMatrix m;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
        m.size = qr.getSize();
        m.dark.assign(static_cast<std::size_t>(m.size) * m.size, 0);
        for (int y = 0; y < m.size; ++y)
            for (int x = 0; x < m.size; ++x) m.dark[static_cast<std::size_t>(y) * m.size + x] = qr.getModule(x, y);
    } catch (const std::exception &) {
        m = QrMatrix(); /* too long for any version */
    }
    return m;
}

int qr_module_px(int modules, int quiet, int box)
{
    const int total = modules + 2 * quiet;
    if (total <= 0) return 1;
    const int px = box / total;
    return px < 1 ? 1 : px;
}

} // namespace ha
