/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: a native fullscreen app on the launcher's cp0_lvgl. The device runner (_common/platform.hpp) does the
 * display, keyboards, signals (SIGTERM / SIGINT / SIGHUP end the loop cleanly) and the root exit; the app is ui/ (the
 * pages), core/ (the recorder), bind/ (the two joined) and demo/ (CASSETTE_DEMO=1, no sound hardware).
 */
#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
