/*
 * SPDX-License-Identifier: MIT
 *
 * mesh-hop-script: Mesh Hop's scripted runner for the simulator flows (the .script files of tests/flows), on the shared headless
 * harness's platform (apps/_common/headless: a plain LVGL display at any size, the layout service's metrics, cp0_lvgl's
 * keyboard queue and bridge, a pointer, the layout checks) with the app started by its own AppSpec (cdk::start, as on
 * a board). Built by tests/flows/runner.mk (build.sh host).
 *
 *   mesh-hop-script --script FILE [--shot-dir DIR] [--size WxH] [--resources DIR]
 *
 * Coordinates in a script are the 640x480 layout's (the stage): at any other size they are moved by the stage's
 * place, so the same script runs at every size. Keys go through cp0_lvgl's keyboard queue and the app's key bridge
 * (stage.hpp), as on a board; `bench` calls the app's key handler directly (the app's own cost per key).
 *
 * Script (one command per line, # starts a comment):
 *   wait <ms>   key <name>   type <text>   tap <x> <y>   tap "label"   hold <x> <y> <ms> [shot]   drag <x1> <y1> <x2> <y2>
 *   bench <key> <n>   shot <name>   dump   sim <line>   say <text>
 *   stage                        the stage's place: must be centred (a failure otherwise)
 *   check <name>                 the shared layout checks: "on-screen" (nothing cut by the screen edge) fails the run;
 *                                the size and header rules are counted only (the pages are not converted yet)
 *   expect text "T" | notext "T" a visible label with exactly this text (or none)
 *   key names: esc enter tab backtab up down left right pgup pgdn home end bksp del, ctrl-<letter>
 */

#include "app.hpp"
#include "headless.hpp"
#include "session.hpp"

#include "keyboard_input.h"

#include <linux/input-event-codes.h>
#include <sys/stat.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace meshhop;

namespace {

std::atomic<bool> g_quit{false};
int g_w = 640, g_h = 480;

void on_signal(int)
{
    g_quit.store(true);
}

void run_for(int ms)
{
    headless::run_for(ms);
}

/* The stage's top-left corner on the screen (0, 0 without an app: the notice). */
lv_point_t stage_origin()
{
    lv_point_t o = {0, 0};
    if (Platform *p = Platform::current())
        if (lv_obj_t *s = p->stage()) {
            lv_area_t a;
            lv_obj_get_coords(s, &a);
            o.x = a.x1;
            o.y = a.y1;
        }
    return o;
}

void touch(bool pressed, int x, int y)
{
    const lv_point_t o = stage_origin();
    headless::platform_touch(pressed, x + o.x, y + o.y);
}

struct KeySpec {
    uint32_t code = 0;
    uint32_t mods = 0;
    std::string utf8;
};

uint32_t letter_code(char c)
{
    static const uint32_t letters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
                                         KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    static const uint32_t digits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ') return KEY_SPACE;
    return 0;                                         // a character without a key of its own: text only
}

bool key_by_name(const std::string &name, KeySpec &k)
{
    k = KeySpec{};
    static const struct { const char *n; uint32_t code; uint32_t mods; } names[] = {
        {"esc", KEY_ESC, 0}, {"enter", KEY_ENTER, 0}, {"tab", KEY_TAB, 0}, {"backtab", KEY_TAB, KBD_MOD_SHIFT},
        {"up", KEY_UP, 0}, {"down", KEY_DOWN, 0}, {"left", KEY_LEFT, 0}, {"right", KEY_RIGHT, 0},
        {"pgup", KEY_PAGEUP, 0}, {"pgdn", KEY_PAGEDOWN, 0}, {"home", KEY_HOME, 0}, {"end", KEY_END, 0},
        {"bksp", KEY_BACKSPACE, 0}, {"del", KEY_DELETE, 0}};
    for (const auto &n : names)
        if (name == n.n) {
            k.code = n.code;
            k.mods = n.mods;
            return true;
        }
    if (name.rfind("ctrl-", 0) == 0 && name.size() == 6) {
        k.code = letter_code(name[5]);
        k.mods = KBD_MOD_CTRL;
        k.utf8 = std::string(1, name[5]);
        return k.code != 0;
    }
    return false;
}

/* The app's KeyEvent for a key name (bench: straight to the app's handler, as the old runner did). */
bool event_by_name(const std::string &name, KeyEvent &e)
{
    e = KeyEvent{};
    static const struct { const char *n; Key k; } names[] = {
        {"esc", Key::Esc}, {"enter", Key::Enter}, {"tab", Key::Tab}, {"backtab", Key::BackTab}, {"up", Key::Up}, {"down", Key::Down},
        {"left", Key::Left}, {"right", Key::Right}, {"pgup", Key::PageUp}, {"pgdn", Key::PageDown}, {"home", Key::Home}, {"end", Key::End},
        {"bksp", Key::Backspace}, {"del", Key::Delete}};
    for (const auto &n : names)
        if (name == n.n) {
            e.key = n.k;
            return true;
        }
    if (name.rfind("ctrl-", 0) == 0 && name.size() == 6) {
        e.key = Key::Char;
        e.ctrl = true;
        e.text = std::string(1, name[5]);
        return true;
    }
    return false;
}

/* A press and a release through cp0_lvgl's keyboard queue (the on-screen keyboard's path): LV_EVENT_KEYBOARD, then the
 * app's key bridge. */
void queue_key(const KeySpec &k)
{
    const char *t = k.utf8.empty() ? nullptr : k.utf8.c_str();
    cp0_keyboard_inject_key(k.code, KBD_KEY_PRESSED, k.mods, t);
    cp0_keyboard_inject_key(k.code, KBD_KEY_RELEASED, k.mods, t);
}

std::vector<std::string> words_of(const std::string &rest)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (const char c : rest) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

bool save_shot(const std::string &shot_dir, const std::string &name)
{
    const std::string path = (shot_dir.empty() ? std::string(".") : shot_dir) + "/" + name + ".png";
    if (!headless::write_png(path, headless::screenshot())) {
        std::fprintf(stderr, "mesh-hop-script: cannot write %s\n", path.c_str());
        return false;
    }
    std::printf("shot %s\n", path.c_str());
    return true;
}

int run_script(const std::string &file, const std::string &shot_dir)
{
    std::ifstream in(file);
    if (!in) {
        std::fprintf(stderr, "mesh-hop: cannot read the script %s\n", file.c_str());
        return 2;
    }
    std::string line;
    int lineno = 0, failures = 0;
    while (std::getline(in, line) && !g_quit.load()) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        Session *session = running_session();
        if (cmd == "wait") {
            int ms = 0;
            ss >> ms;
            run_for(ms);
        } else if (cmd == "key") {
            std::string name;
            ss >> name;
            KeySpec k;
            if (!key_by_name(name, k)) {
                std::fprintf(stderr, "script:%d: unknown key %s\n", lineno, name.c_str());
                ++failures;
            } else {
                queue_key(k);
            }
            run_for(60);
        } else if (cmd == "type") {
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            for (size_t i = 0; i < text.size();) {
                size_t n = 1;
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (c >= 0xF0) n = 4;
                else if (c >= 0xE0) n = 3;
                else if (c >= 0xC0) n = 2;
                KeySpec k;
                k.utf8 = text.substr(i, n);
                k.code = n == 1 ? letter_code(text[i]) : 0;
                queue_key(k);
                i += n;
            }
            run_for(60);
        } else if (cmd == "tap") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            if (w.size() == 1) {                                  // tap "label": the middle of its clickable parent
                lv_obj_t *label = headless::find_label(w[0], false);
                if (!label) {
                    std::fprintf(stderr, "script:%d: nothing labelled \"%s\" to tap\n", lineno, w[0].c_str());
                    ++failures;
                    continue;
                }
                const lv_area_t a = headless::visible_area(headless::clickable_of(label));
                const int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
                headless::platform_touch(true, x, y);
                run_for(80);
                headless::platform_touch(false, x, y);
                run_for(80);
            } else {
                const int x = w.size() > 0 ? std::atoi(w[0].c_str()) : 0, y = w.size() > 1 ? std::atoi(w[1].c_str()) : 0;
                touch(true, x, y);
                run_for(80);
                touch(false, x, y);
                run_for(80);
            }
        } else if (cmd == "hold") {
            // hold <x> <y> <ms> [shot-name]: a finger kept down; with a name, a screenshot is taken at the end of the hold (before the release)
            int x = 0, y = 0, ms = 0;
            std::string shot;
            ss >> x >> y >> ms >> shot;
            touch(true, x, y);
            run_for(ms);
            if (!shot.empty() && !save_shot(shot_dir, shot)) ++failures;
            touch(false, x, y);
            run_for(150);
        } else if (cmd == "drag") {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            ss >> x1 >> y1 >> x2 >> y2;
            touch(true, x1, y1);
            run_for(60);
            for (int i = 1; i <= 8; ++i) {
                touch(true, x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8);
                run_for(30);
            }
            touch(false, x2, y2);
            run_for(100);
        } else if (cmd == "bench") {
            // bench <key> <n>: n key presses, each followed by one pass of the app's tick and LVGL (no sleeping): the milliseconds per press
            std::string name;
            int n = 0;
            ss >> name >> n;
            KeyEvent e;
            if (!event_by_name(name, e) || n <= 0 || !session) {
                std::fprintf(stderr, "script:%d: bench <key> <n>\n", lineno);
                ++failures;
            } else {
                const uint64_t t0 = mono_ms();
                for (int i = 0; i < n; ++i) {
                    session->platform().inject_key(e);
                    session->app().tick();
                    lv_timer_handler();
                }
                const uint64_t dt = mono_ms() - t0;
                std::printf("bench %s x%d: %llu ms, %.2f ms per key\n", name.c_str(), n, static_cast<unsigned long long>(dt),
                            static_cast<double>(dt) / n);
                run_for(100);
            }
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            run_for(120);                                     // let the last changes be drawn
            if (!save_shot(shot_dir, name)) {
                std::fprintf(stderr, "script:%d: shot %s failed\n", lineno, name.c_str());
                ++failures;
            }
        } else if (cmd == "dump") {
            run_for(60);
            if (session) std::printf("%s\n", session->app().debug_state().c_str());
            else std::printf("state none (the app is not running: notice)\n");
        } else if (cmd == "sim") {
            // sim <line>: a line for the simulated board's stdin ("swap", "unplug", "plug", "msg text"...), through the fifo named by MESHHOP_SIMIN
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            const char *fifo = std::getenv("MESHHOP_SIMIN");
            std::ofstream out(fifo ? fifo : "/dev/null", std::ios::app);
            if (!fifo || !out) {
                std::fprintf(stderr, "script:%d: sim needs MESHHOP_SIMIN (a fifo read by the simulator)\n", lineno);
                ++failures;
            } else {
                out << text << '\n';
            }
        } else if (cmd == "say") {
            std::string text;
            std::getline(ss, text);
            std::printf("%s\n", text.c_str() + (text.empty() ? 0 : 1));
        } else if (cmd == "stage") {
            run_for(60);
            const lv_point_t o = stage_origin();
            const int want_x = (g_w - kScreenW) / 2, want_y = (g_h - kScreenH) / 2;
            const bool ok = session && o.x == want_x && o.y == want_y;
            std::printf("stage %s: %dx%d at (%d,%d) on %dx%d, centred at (%d,%d)\n", ok ? "PASS" : "FAIL", kScreenW, kScreenH,
                        static_cast<int>(o.x), static_cast<int>(o.y), g_w, g_h, want_x, want_y);
            if (!ok) ++failures;
        } else if (cmd == "check") {
            std::string name;
            ss >> name;
            run_for(250);
            const headless::CheckResult r = headless::check_layout(!session);
            int onscreen = 0, other = 0;
            for (const std::string &f : r.failures) {
                if (f.rfind("on-screen", 0) == 0) {
                    ++onscreen;
                    std::printf("check %s FAIL %s\n", name.c_str(), f.c_str());
                } else {
                    ++other;
                }
            }
            std::printf("check %s %s: on-screen %d failure(s); %d size/header finding(s) (pages not converted yet)", name.c_str(),
                        onscreen ? "FAIL" : "PASS", onscreen, other);
            for (const std::string &n : r.notes) std::printf(" | %s", n.c_str());
            std::printf("\n");
            failures += onscreen;
        } else if (cmd == "expect") {
            std::string rest;
            std::getline(ss, rest);
            const std::vector<std::string> w = words_of(rest);
            const std::string what = w.empty() ? std::string() : w[0], arg = w.size() > 1 ? w[1] : std::string();
            run_for(60);
            bool ok = true;
            if (what == "text") ok = headless::find_label(arg, false) != nullptr;
            else if (what == "notext") ok = headless::find_label(arg, false) == nullptr;
            else {
                std::fprintf(stderr, "script:%d: expect text|notext \"T\"\n", lineno);
                ok = false;
            }
            std::printf("expect %s \"%s\": %s\n", what.c_str(), arg.c_str(), ok ? "PASS" : "FAIL");
            if (!ok) ++failures;
        } else {
            std::fprintf(stderr, "script:%d: unknown command %s\n", lineno, cmd.c_str());
            ++failures;
        }
        std::fflush(stdout);
    }
    return failures ? 1 : 0;
}

/* The densities of the shared harness (and the launcher's render harness), so both compute the same tokens. */
int ppmm_for(int w, int h)
{
    if (w == 800 && h == 480) return 920;
    if (w == 720 && h == 720) return 1000;
    if (w == 1280 && h == 720) return 1160;
    return 1130;
}

void mkdirs(const std::string &path)
{
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        mkdir(cur.c_str(), 0755);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    std::string script, shot_dir, resources;
    if (const char *e = std::getenv("CDK_HEADLESS_RESOURCES")) resources = e;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") continue;                      // the old runner's flag: always headless here
        else if (a == "--script" && i + 1 < argc) script = argv[++i];
        else if (a == "--shot-dir" && i + 1 < argc) shot_dir = argv[++i];
        else if (a == "--resources" && i + 1 < argc) resources = argv[++i];
        else if (a == "--size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &g_w, &g_h) != 2 || g_w <= 0 || g_h <= 0) {
                std::fprintf(stderr, "--size WxH\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "usage: %s --script file [--shot-dir dir] [--size WxH] [--resources dir]\n", argv[0]);
            return a == "--help" ? 0 : 2;
        }
    }
    if (script.empty()) {
        std::fprintf(stderr, "--script is needed\n");
        return 2;
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    // the per-launch environment of a fullscreen app (as the shared harness sets it), never the real runtime directory
    const std::string runtime = (shot_dir.empty() ? std::string("/tmp") : shot_dir) + "/.runtime/applaunch";
    mkdirs(runtime);
    setenv("APPLAUNCH_STATE_DIR", runtime.c_str(), 1);
    const std::string size = std::to_string(g_w) + "x" + std::to_string(g_h);
    setenv("APPLAUNCH_CONTRACT", "1", 1);
    setenv("APPLAUNCH_APP_MODE", "fullscreen", 1);
    setenv("APPLAUNCH_WINDOW", ("0,0," + size.substr(0, size.find('x')) + "," + std::to_string(g_h)).c_str(), 1);
    setenv("APPLAUNCH_CANVAS", size.c_str(), 1);
    setenv("APPLAUNCH_CANVAS_SCALE", "1", 1);

    if (!headless::platform_init(g_w, g_h, ppmm_for(g_w, g_h), resources)) {
        std::fprintf(stderr, "mesh-hop-script: platform init failed\n");
        return 1;
    }
    Platform::set_keyboard_assumed(true);                     // as the old headless mode: typing is never blocked
    const cdk::AppSpec &spec = cdk::app_spec();
    cdk::start(spec, lv_screen_active());
    const int rc = run_script(script, shot_dir);
    cdk::stop();
    return rc;
}
