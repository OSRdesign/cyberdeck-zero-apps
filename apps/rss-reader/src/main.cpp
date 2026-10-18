/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: a text-feed reader, a native fullscreen app (X-Fullscreen=true) on the launcher's cp0_lvgl. The device
 * runner (apps/_common/platform.hpp) does the display, the touch, the keyboards, the signals (the launcher's 3 s Esc
 * hold ends the app with SIGTERM) and the "needs 640x480" notice; the app is ui/ (the pages) and core/ (feeds, the
 * catalog, the store, HTTP through curl).
 *
 *   rss-reader             started by the launcher, or by hand on the board's console
 *   rss-reader --version   the version, the launcher commit of the build
 *
 * On the PC: the shared headless harness (rss-reader-headless, tests/scenes) and the scripted flows (rss-reader-script,
 * tests/flows): build.sh host.
 */

#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
