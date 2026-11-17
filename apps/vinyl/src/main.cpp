/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl: a music player, a native fullscreen app on the launcher's cp0_lvgl. The device runner (_common/platform.hpp)
 * does the display, keyboards, signals (SIGTERM / SIGINT / SIGHUP end the loop cleanly) and the multitasking lifecycle
 * (X-Background=run: the music goes on while hidden); the app is ui/ (the pages and the controller) and core/ (the
 * decoders, the engine, the outputs, the queue, the library; LVGL-free and unit-tested). Knock-knock's screen request is
 * the toolkit's cp0_app_backlight (cp0_app_client.h, launcher.sock `screen off|on`), given here because only the device
 * build links the client (the headless harness has none).
 */
#include "platform.hpp"
#include "vinyl_app.hpp"
#include "engine/dsp_bench.hpp"

#include "cp0_app_client.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    /* P2d: `vinyl --bench-dsp` prints the CPU of the EQ, the limiter and the crossfade on 60 s of audio (board test B87),
     * without a screen */
    if (argc > 1 && std::strcmp(argv[1], "--bench-dsp") == 0) {
        std::fputs(vinyl::dsp_bench_text(vinyl::run_dsp_bench(60.0)).c_str(), stdout);
        return 0;
    }
    vinyl::ui::set_backlight_call(cp0_app_backlight);
    return cdk::run(cdk::app_spec(), argc, argv);
}
