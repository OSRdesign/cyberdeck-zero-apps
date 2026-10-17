/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop: full-screen MeshCore messenger, a native fullscreen app (X-Fullscreen=true) on the launcher's cp0_lvgl.
 * The device runner (apps/_common/platform.hpp) does the display, the touch, the keyboards, the signals (the launcher's
 * 3 s Esc hold ends the app with SIGTERM) and the "needs 640x480" notice; the app is ui/ (the key bridge, the screens) and
 * core/ (the radio client, the model, the store).
 *
 *   mesh-hop             started by the launcher, or by hand on the board's console
 *   mesh-hop --version   the version, the launcher commit of the build
 *
 * On the PC: the shared headless harness (mesh-hop-headless, tests/scenes) and the scripted simulator flows
 * (mesh-hop-script, tests/flows): build.sh host.
 */

#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
