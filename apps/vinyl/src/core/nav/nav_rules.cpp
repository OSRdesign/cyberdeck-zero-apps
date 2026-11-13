/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl navigation rules (see nav_rules.hpp).
 */
#include "nav_rules.hpp"

namespace vinyl {

BackStep back_step(bool modal_shown, bool notice_shown) {
    if (modal_shown) return BackStep::CloseModal;
    return notice_shown ? BackStep::HideNoticeAndBack : BackStep::Back;
}

ModalAnswer modal_answer(uint32_t lv_key, uint32_t evdev) {
    if (evdev == kEvdevKeyY) return ModalAnswer::Confirm;
    if (lv_key == kLvKeyEnter || lv_key == kLvKeyEsc) return ModalAnswer::Cancel;
    return ModalAnswer::None;
}

unsigned queue_buttons_enabled(int queue_size) {
    if (queue_size <= 0) return 0;
    if (queue_size == 1) return kQueueRemove; /* only the playing track: nothing to move, nothing to clear */
    return kQueueUp | kQueueDown | kQueueRemove | kQueueClear;
}

}  // namespace vinyl
