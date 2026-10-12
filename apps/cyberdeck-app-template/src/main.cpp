/*
 * SPDX-License-Identifier: MIT
 *
 * Cyberdeck app template: a native fullscreen app (X-Fullscreen=true) on the launcher's cp0_lvgl. The device runner
 * (_common/platform.hpp) does the display, keyboards, signals and the root exit; the app is ui/ and core/.
 */
#include "platform.hpp"

int main(int argc, char **argv)
{
    return cdk::run(cdk::app_spec(), argc, argv);
}
