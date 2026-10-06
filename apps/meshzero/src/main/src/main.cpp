/*
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app_runner.hpp"
#include "meshzero.hpp"

#include <utility>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    Cp0LvglAppHooks hooks;
    hooks.should_quit = [] { return meshzero_quit_requested(); };
    return cp0_lvgl_run_app<UIMeshZeroPage>(std::move(hooks));
}
