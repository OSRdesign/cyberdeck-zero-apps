/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the template's core/ (no LVGL, no screen) and of its package files (.desktop, app.json).
 * Built and run by ../../_common/build_app.sh host (make -C _common/headless).
 */
#include "template_core.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CDK_APP_DIR
#define CDK_APP_DIR "."
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

static std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void test_nav()
{
    tpl::Nav nav;
    CHECK(nav.current() == tpl::View::Root);
    CHECK(nav.title_path("Template") == std::vector<std::string>{"Template"});
    nav.open(tpl::View::Info);
    CHECK(nav.depth() == 2);
    CHECK((nav.title_path("Template") == std::vector<std::string>{"Template", "Device info"}));
    CHECK(nav.back() == tpl::Back::Popped);
    CHECK(nav.current() == tpl::View::Root);
    CHECK(nav.back() == tpl::Back::Quit); /* a short Esc at the root ends the app */
    CHECK(nav.depth() == 1);
}

static void test_name_rules()
{
    CHECK(tpl::trim("  Deck one \t") == "Deck one");
    CHECK(tpl::trim("   ").empty());
    CHECK(tpl::utf8_length("D\xC3\xA9" "ck") == 4); /* é is one character */
    CHECK(tpl::utf8_length("\xE2\x82\xAC") == 1);
    CHECK(tpl::utf8_length("\xC3") == 1);           /* a cut sequence still counts */
    CHECK(tpl::name_error("").size() > 0);
    CHECK(tpl::name_error("Deck").empty());
    CHECK(tpl::name_error("D\xC3\xA9" "ck").empty());
    CHECK(!tpl::name_error("tab\there").empty());
    CHECK(tpl::name_error(std::string(24, 'x')).empty());
    CHECK(!tpl::name_error(std::string(25, 'x')).empty());
}

static void test_run_state()
{
    tpl::RunState s;
    CHECK(s.phase() == tpl::Phase::Idle);
    CHECK(s.value().empty());
    CHECK(!s.busy());
    s.begin();
    CHECK(s.busy());
    CHECK(s.busy_label() == "Starting...");
    s.line("step 1 of 5");
    s.line("   ");
    CHECK(s.busy_label() == "step 1 of 5");
    CHECK(s.lines() == 2);
    s.finish(true, 0, 0, false, false);
    CHECK(s.phase() == tpl::Phase::Done);
    CHECK(s.value() == "Done, 2 lines");
    CHECK(!s.failed());

    s.begin();
    s.line("x");
    s.finish(true, 2, 0, false, false);
    CHECK(s.value() == "Failed (exit 2)");
    CHECK(s.failed());
    s.begin();
    s.finish(true, -1, 15, true, false);
    CHECK(s.phase() == tpl::Phase::TimedOut);
    CHECK(s.value() == "Timed out");
    s.begin();
    s.finish(true, -1, 15, false, true);
    CHECK(s.phase() == tpl::Phase::Stopped);
    CHECK(!s.failed());
    s.begin();
    s.finish(false, -1, 0, false, false);
    CHECK(s.value() == "Failed");
    s.begin();
    s.line("only");
    s.finish(true, 0, 0, false, false);
    CHECK(s.value() == "Done, 1 line");

    const std::vector<std::string> argv = tpl::demo_command();
    CHECK(argv.size() == 3 && argv[0] == "sh");
}

static void test_info()
{
    CHECK(tpl::format_ppmm(1130) == "11.30 px/mm");
    CHECK(tpl::format_ppmm(920) == "9.20 px/mm");
    tpl::Facts f;
    f.w = 640;
    f.h = 480;
    f.cls = "standard";
    f.ppmm_x100 = 1130;
    f.row = 79;
    f.target = 102;
    f.header = 56;
    f.contract = true;
    f.mode = "fullscreen";
    f.version = "0.1.0";
    f.launcher_ref = "2b8cb96 (pinned)";
    CHECK(tpl::screen_summary(f) == "640x480 standard");
    const auto items = tpl::info_items(f);
    bool screen = false, keyboards = false, launcher = false;
    for (const auto &kv : items) {
        if (kv.first == "Screen") screen = kv.second == "640x480 standard";
        if (kv.first == "Keyboards") keyboards = kv.second == "no input.state";
        if (kv.first == "Launcher") launcher = kv.second == "contract 1, fullscreen";
    }
    CHECK(screen && keyboards && launcher);
    f.input_file = true;
    f.keyboards = 2;
    f.vkb = "force";
    bool rule = false;
    for (const auto &kv : tpl::info_items(f))
        if (kv.first == "Keyboard rule") rule = kv.second == "force";
    CHECK(rule);
}

/* The package: X-Fullscreen, X-MinSize equal to the app's minimum, Exec and Icon pointing at files of root/, draft. */
static void test_package()
{
    const std::string app = CDK_APP_DIR;
    const std::string desktop =
        read_file(app + "/root/usr/share/APPLaunch/applications/cyberdeck-app-template.desktop");
    CHECK(!desktop.empty());
    CHECK(desktop.find("\nX-Fullscreen=true") != std::string::npos);
    const std::string min = "\nX-MinSize=" + std::to_string(tpl::kMinW) + "x" + std::to_string(tpl::kMinH);
    CHECK(desktop.find(min) != std::string::npos);
    CHECK(desktop.find("\nExec=/usr/share/APPLaunch/bin/cyberdeck-app-template") != std::string::npos);
    CHECK(desktop.find("\nIcon=share/images/cyberdeck-app-template.png") != std::string::npos);
    CHECK(desktop.find("\nX-Background=freeze\n") != std::string::npos); /* multitasking: the cheap default */
    CHECK(!read_file(app + "/root/usr/share/APPLaunch/share/images/cyberdeck-app-template.png").empty());
    CHECK(!read_file(app + "/root/usr/share/doc/cyberdeck-app-template/copyright").empty());
    const std::string manifest = read_file(app + "/app.json");
    CHECK(manifest.find("\"draft\": true") != std::string::npos);
    CHECK(manifest.find("\"share_code\": \"cyberdeck-app-template\"") != std::string::npos);
}

int main()
{
    test_nav();
    test_name_rules();
    test_run_state();
    test_info();
    test_package();
    std::printf("test-cyberdeck-app-template: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
