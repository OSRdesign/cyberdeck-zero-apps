/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: notes as plain Markdown files in a folder (the vault, ~/Notes by default), a native fullscreen app
 * (X-Fullscreen=true) on the launcher's cp0_lvgl. The device runner (apps/_common/platform.hpp) does the display, the
 * touch, the keyboards, the signals (the launcher's 3 s Esc hold ends the app with SIGTERM) and the "needs 640x480"
 * notice; the app is ui/ (the pages) and core/ (the vault, the files, names, links, the Markdown model on md4c).
 *
 *   markdown-notes             started by the launcher, or by hand on the board's console
 *   markdown-notes --version   the version, the launcher commit of the build
 *
 * On the PC: the shared headless harness (markdown-notes-headless, tests/scenes) and the unit tests (tests/unit):
 * build.sh host.
 */

#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
