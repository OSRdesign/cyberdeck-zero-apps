/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl: a music player, a native fullscreen app on the launcher's cp0_lvgl. The device runner (_common/platform.hpp)
 * does the display, keyboards, signals (SIGTERM / SIGINT / SIGHUP end the loop cleanly) and the multitasking lifecycle
 * (X-Background=run: the music goes on while hidden); the app is ui/ (the pages and the controller) and core/ (the
 * decoders, the engine, the outputs, the queue, the library; LVGL-free and unit-tested).
 */
#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
