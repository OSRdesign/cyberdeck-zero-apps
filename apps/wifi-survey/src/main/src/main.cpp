/*
 * SPDX-License-Identifier: MIT
 */

#include "cp0_lvgl_app_runner.hpp"
#include "wifisurvey.hpp"

#include <utility>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    Cp0LvglAppHooks hooks;
    hooks.should_quit = [] { return wifisurvey_quit_requested(); };
    return cp0_lvgl_run_app<UIWifiSurveyPage>(std::move(hooks));
}
