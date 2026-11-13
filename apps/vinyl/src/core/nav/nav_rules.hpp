/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl navigation rules (design QA run 4), pure and tested (tests/test_queue_browse.cpp):
 *
 *   back_step               what Esc and the header's Esc do: a modal (the "Clear the queue?" dialog) takes them
 *                           until it is closed (D-30: it is cancelled, nothing else happens); otherwise a notice goes
 *                           with the step back, and the step back is the page's (at the browser: quit);
 *   queue_buttons_enabled   which of the queue page's buttons can act (D-81: the others are drawn dimmed and do
 *                           nothing): none with an empty queue; with only the playing track, Remove only;
 *   modal_answer            what a key does while "Clear the queue?" shows (design QA run 5, the toolkit's dialog
 *                           unrestyled): Enter and Esc cancel (Cancel is the dialog's Primary: the owner's "yes default
 *                           cancel"), the letter Y and nothing else clears; no other key does anything (no hidden
 *                           highlight to move).
 */
#pragma once

#include <cstdint>

namespace vinyl {

enum class BackStep { CloseModal, HideNoticeAndBack, Back };
BackStep back_step(bool modal_shown, bool notice_shown);

/* bit 0 Move up, bit 1 Move down, bit 2 Remove, bit 3 Clear all */
constexpr unsigned kQueueUp = 1u, kQueueDown = 2u, kQueueRemove = 4u, kQueueClear = 8u;
unsigned queue_buttons_enabled(int queue_size);

enum class ModalAnswer { None, Cancel, Confirm };
/* lv_key: the LVGL key of the keypad (LV_KEY_ESC 27, LV_KEY_ENTER 10); evdev: the physical key (KEY_Y 21), 0 if none */
ModalAnswer modal_answer(uint32_t lv_key, uint32_t evdev);
constexpr uint32_t kLvKeyEnter = 10, kLvKeyEsc = 27, kEvdevKeyY = 21;

}  // namespace vinyl
