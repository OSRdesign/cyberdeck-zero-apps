/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the handwriting fonts (see handwriting.hpp).
 */
#include "handwriting.hpp"

#include "load_anim.hpp"
#include "paint.hpp"

extern "C" {
extern const lv_font_t caveat_semibold_15;
extern const lv_font_t caveat_semibold_22;
extern const lv_font_t caveat_semibold_29;
extern const lv_font_t caveat_semibold_31;
}

namespace cassette {
namespace ui {

const lv_font_t *handwriting_font(int rung)
{
    static const lv_font_t *const base[kHandRungCount] = {&caveat_semibold_15, &caveat_semibold_22, &caveat_semibold_29,
                                                          &caveat_semibold_31};
    /* the generated fonts are const and have no fallback: copies with the Bold face of the same size behind them */
    static lv_font_t copies[kHandRungCount];
    static bool made[kHandRungCount] = {};
    const int r = hand_rung(rung);
    for (int i = 0; i < kHandRungCount; ++i) {
        if (kHandRungs[i] != r) continue;
        if (!made[i]) {
            copies[i] = *base[i];
            copies[i].fallback = font(Face::Bold, r);
            made[i] = true;
        }
        return &copies[i];
    }
    return &copies[0];
}

} // namespace ui
} // namespace cassette
