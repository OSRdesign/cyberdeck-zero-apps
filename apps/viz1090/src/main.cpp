/*
 * SPDX-License-Identifier: MIT
 *
 * vizsetup: the intro screen of viz1090 (/opt/viz1090/vizsetup), a native fullscreen app on the launcher's cp0_lvgl.
 * The device runner (_common/platform.hpp) does the display, keyboards, signals and the "needs 640x480" notice; the app
 * is ui/ and core/. The contract with run_viz1090.sh:
 *
 *   vizsetup            the intro screen; exit status 0 = Start (the position is saved in
 *                       ~/.config/cardputerzero/viz1090.conf: start the map), 1 = cancelled (Esc at the root, the notice
 *                       of a too small screen, a signal), 2 or more = could not run
 *   vizsetup --screen   prints "W H UISCALE PPMM_X100 SOURCE" (the screen viz1090 and its bridge use, viz_screen.h), exits
 *   vizsetup --version  the version and the launcher it was built against
 */
#include "platform.hpp"
#include "viz_app.hpp"
#include "viz_screen.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--screen") == 0) {
        viz_screen_t screen;
        viz_screen_detect(&screen);
        std::printf("%d %d %d %d %s\n", screen.w, screen.h, viz_screen_uiscale(&screen), screen.ppmm_x100, screen.source);
        return 0;
    }
    const int rc = cdk::run(cdk::app_spec(), argc, argv);
    if (argc > 1) return rc; /* --version, --help, a wrong argument */
    if (viz::start_confirmed()) return 0;
    return rc > 1 ? rc : 1;
}
