/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness: the keyboard backend's globals and its injection entry point.
 *
 * On a board cp0_lvgl_keyboard.c reads libinput + xkbcommon on a thread and queues key_items; that file needs
 * libinput, so the harness provides what it defines (same approach as the launcher's render harness). Everything
 * downstream is cp0_lvgl's real code: the queue (cp0_keyboard_queue.c), cp0_keyboard_inject_key() (the on-screen
 * keyboard's path, used here for typed text) and the LVGL bridge (cp0_keyboard_lvgl_input.c), which sends
 * LV_EVENT_KEYBOARD to the active screen and feeds the keypad indev.
 */
#include "cp0_keyboard_navigation_contract.h"
#include "cp0_keyboard_key_contract.h"
#include "cp0_keyboard_queue.h"
#include "cp0/cp0_keyboard_lvgl_input.h"
#include "keyboard_input.h"
#include "lvgl/lvgl.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct keyboard_queue_t keyboard_queue;
pthread_mutex_t keyboard_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int LVGL_RUN_FLAGE = 1;
volatile uint32_t LV_EVENT_KEYBOARD;

static cp0_keyboard_input_context_t input_context = KBD_INPUT_CONTEXT_NAVIGATION;
static atomic_int read_all = 0;

void cp0_keyboard_set_input_context(cp0_keyboard_input_context_t context)
{
    if (context < KBD_INPUT_CONTEXT_NAVIGATION || context > KBD_INPUT_CONTEXT_GAME) context = KBD_INPUT_CONTEXT_NAVIGATION;
    input_context = context;
}

cp0_keyboard_input_context_t cp0_keyboard_get_input_context(void) { return input_context; }

void cp0_keyboard_set_read_all_keyboards(int enable) { atomic_store(&read_all, enable != 0); }
int cp0_keyboard_get_read_all_keyboards(void) { return atomic_load(&read_all); }

/* Same semantics as the device's cp0_keyboard_inject(): a key without text (context, semantic key, control UTF-8). */
int cp0_keyboard_inject(uint32_t key_code, int key_state, uint32_t mods)
{
    if (key_state != KBD_KEY_RELEASED && key_state != KBD_KEY_PRESSED && key_state != KBD_KEY_REPEATED) return -1;
    struct key_item item;
    memset(&item, 0, sizeof(item));
    item.key_code = key_code;
    item.key_state = key_state;
    item.mods = mods;
    item.input_context = cp0_keyboard_get_input_context();
    item.semantic_key = cp0_keyboard_semantic_key(key_code, item.input_context);
    const char *ctrl = cp0_keyboard_control_utf8(key_code);
    if (ctrl) snprintf(item.utf8, sizeof(item.utf8), "%s", ctrl);
    snprintf(item.sym_name, sizeof(item.sym_name), "HEADLESS_%u", key_code);
    return cp0_keyboard_queue_push(&item) == 0 ? 0 : -1;
}

void headless_keyboard_init(void)
{
    cp0_keyboard_queue_init();
    if (LV_EVENT_KEYBOARD == 0) LV_EVENT_KEYBOARD = lv_event_register_id();
    cp0_keyboard_create_lvgl_input_devices();
}
