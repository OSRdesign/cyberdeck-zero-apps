/*
 * SPDX-License-Identifier: MIT
 *
 * Headless harness of the cdk apps (see README.md):
 *
 *   <app>-headless --scene FILE [--sizes WxH,WxH,...] [--out DIR] [--resources DIR]
 *
 * Every size runs in its own forked process (LVGL and the layout service are process-wide), all at once: a plain LVGL
 * display at that size, the app built by its own AppSpec (cdk::start, as on the deck), the scene played, PNG shots
 * with the layout checks. Then one contact sheet per scene. Exit status 0 when every size passed.
 */
#include "headless.hpp"

#include "app.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_widgets.hpp"
#include "keyboard_input.h"

#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Size {
    int w = 0, h = 0;
    std::string name() const { return std::to_string(w) + "x" + std::to_string(h); }
};

bool parse_size(const std::string &s, Size &out)
{
    return std::sscanf(s.c_str(), "%dx%d", &out.w, &out.h) == 2 && out.w > 0 && out.h > 0;
}

/* The densities of the launcher's render harness (generic profile), so both harnesses compute the same tokens. The
 * portrait canvases (PDA mode, an app with X-Portrait) are the panels of the launcher's PDA scenes, turned: 480x800 the
 * Waveshare 4.3" DSI (94x56 mm), 480x640 a 3.5" 640x480 panel (71x53 mm), 800x1280 the FZ-M1 tablet (150x93 mm). */
int default_ppmm(const Size &s)
{
    if (s.w == 800 && s.h == 480) return 920;
    if (s.w == 720 && s.h == 720) return 1000;
    if (s.w == 1280 && s.h == 720) return 1160;
    if (s.w == 480 && s.h == 800) return 851;
    if (s.w == 480 && s.h == 640) return 901;
    if (s.w == 800 && s.h == 1280) return 856;
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

std::vector<std::string> split_words(const std::string &line)
{
    std::vector<std::string> words;
    std::string cur;
    bool quoted = false, any = false;
    for (const char c : line) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) words.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any) words.push_back(cur);
    return words;
}

struct Line {
    int number = 0;
    std::vector<Size> only;
    std::vector<std::string> words;
};

struct Scene {
    std::string path, name, title;
    std::vector<Size> sizes;
    std::vector<Line> lines;
    bool nocontract = false; /* started by hand: no contract variables, no input.state (APP-CONTRACT section 7) */
    bool lifecycle = false;  /* a multitasking launcher: the control socket (APP-CONTRACT section 13) */
    bool band = false;       /* a launcher with the PDA writing zone runs the app upright: APPLAUNCH_PDA=1, hand=1 */
    std::vector<std::pair<std::string, std::string>> env; /* set before the app starts (an app's test fixture) */
};

bool load_scene(const std::string &path, Scene &scene)
{
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    scene.path = path;
    std::string base = path.substr(path.find_last_of('/') + 1);
    scene.name = base.substr(0, base.find('.'));
    scene.title = scene.name;
    std::string text;
    int number = 0;
    while (std::getline(in, text)) {
        ++number;
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        Line line;
        line.number = number;
        line.words = split_words(text);
        if (line.words.empty() || line.words[0][0] == '#') continue;
        if (line.words[0][0] == '@') {
            std::stringstream list(line.words[0].substr(1));
            std::string item;
            while (std::getline(list, item, ',')) {
                Size s;
                if (!parse_size(item, s)) {
                    std::fprintf(stderr, "%s:%d: bad size %s\n", path.c_str(), number, item.c_str());
                    return false;
                }
                line.only.push_back(s);
            }
            line.words.erase(line.words.begin());
            if (line.words.empty()) continue;
        }
        if (line.words[0] == "title" && line.words.size() > 1) {
            scene.title.clear();
            for (size_t i = 1; i < line.words.size(); ++i) scene.title += (i > 1 ? " " : "") + line.words[i];
            continue;
        }
        if (line.words[0] == "nocontract") {
            scene.nocontract = true;
            continue;
        }
        if (line.words[0] == "lifecycle") {
            scene.lifecycle = true;
            continue;
        }
        if (line.words[0] == "band") {
            scene.band = true;
            continue;
        }
        if (line.words[0] == "env") {
            if (line.words.size() != 3) {
                std::fprintf(stderr, "%s:%d: env NAME VALUE\n", path.c_str(), number);
                return false;
            }
            scene.env.emplace_back(line.words[1], line.words[2]);
            continue;
        }
        if (line.words[0] == "sizes") {
            for (size_t i = 1; i < line.words.size(); ++i) {
                Size s;
                if (parse_size(line.words[i], s)) scene.sizes.push_back(s);
            }
            continue;
        }
        scene.lines.push_back(line);
    }
    return true;
}

/* ------------------------------------------------------------------ one size (child process) */

struct Run {
    Size size;
    std::string out;     /* out/<WxH> */
    std::string runtime; /* the state directory */
    int keyboards = 0;
    std::string vkb = "auto";
    int hand = 0; /* input.state hand= (report 044): Handwriting + keyboard */
    unsigned gen = 0;
    bool notice = false;
    int failures = 0;
    std::vector<std::string> shots;
    std::ofstream report;
};

void write_input_state(Run &run)
{
    ++run.gen;
    const std::string path = run.runtime + "/input.state";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        f << "v=1\nkeyboards=" << run.keyboards << "\nlast_seen=0\nbt_keyboards=0\nkb_gone=0\nbt_gone=0\nvkb="
          << run.vkb << "\nhand=" << run.hand << "\ngen=" << run.gen << "\n";
    }
    std::rename(tmp.c_str(), path.c_str());
}

void fail(Run &run, const Line &line, const std::string &what)
{
    ++run.failures;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "line %d: ", line.number);
    run.report << "FAIL " << buf << what << "\n";
    std::fprintf(stderr, "[%s] FAIL %s%s\n", run.size.name().c_str(), buf, what.c_str());
}

uint32_t key_code(const std::string &name, std::string &utf8)
{
    static const std::map<std::string, uint32_t> named = {
        {"esc", KEY_ESC},     {"enter", KEY_ENTER}, {"up", KEY_UP},         {"down", KEY_DOWN},
        {"left", KEY_LEFT},   {"right", KEY_RIGHT}, {"tab", KEY_TAB},       {"backspace", KEY_BACKSPACE},
        {"delete", KEY_DELETE}, {"space", KEY_SPACE}, {"home", KEY_HOME},   {"end", KEY_END},
        {"pgup", KEY_PAGEUP}, {"pgdn", KEY_PAGEDOWN}};
    utf8.clear();
    const auto it = named.find(name);
    if (it != named.end()) {
        if (name == "space") utf8 = " ";
        return it->second;
    }
    if (name.size() == 1) {
        static const uint32_t letters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
                                             KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
                                             KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        static const uint32_t digits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
        const char c = name[0];
        utf8 = name;
        if (c >= 'a' && c <= 'z') return letters[c - 'a'];
        if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
        if (c >= '0' && c <= '9') return digits[c - '0'];
        return 0; /* a character without a key of its own: text only */
    }
    return 0;
}

void press(uint32_t code, const std::string &utf8)
{
    cp0_keyboard_inject_key(code, KBD_KEY_PRESSED, 0, utf8.empty() ? nullptr : utf8.c_str());
    headless::run_for(30);
    cp0_keyboard_inject_key(code, KBD_KEY_RELEASED, 0, utf8.empty() ? nullptr : utf8.c_str());
    headless::run_for(50);
}

void tap_at(int x, int y)
{
    headless::platform_touch(true, x, y);
    headless::run_for(60);
    headless::platform_touch(false, x, y);
    headless::run_for(120);
}

void shot(Run &run, const Line &line, const std::string &name, bool save)
{
    headless::run_for(250); /* settle: animations, the drain of posted work */
    if (save) {
        const headless::Image img = headless::screenshot();
        const std::string path = run.out + "/" + name + ".png";
        if (!headless::write_png(path, img)) fail(run, line, "cannot write " + path);
        run.shots.push_back(name);
    }
    const headless::CheckResult r = headless::check_layout(run.notice);
    for (const std::string &f : r.failures) fail(run, line, (save ? "shot " : "check ") + name + ": " + f);
    run.report << (r.failures.empty() ? "PASS " : "FAIL ") << (save ? "shot " : "check ") << name;
    for (const std::string &n : r.notes) run.report << " | " << n;
    run.report << "\n";
}

void play(Run &run, const Scene &scene)
{
    for (const Line &line : scene.lines) {
        if (!line.only.empty() &&
            std::none_of(line.only.begin(), line.only.end(),
                         [&](const Size &s) { return s.w == run.size.w && s.h == run.size.h; }))
            continue;
        const std::vector<std::string> &w = line.words;
        const std::string &cmd = w[0];
        /* the app asked to end: on the deck the run loop stops there; only expectations are read further */
        if (cdk::quit_requested() && cmd != "expect") continue;
        const auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
        if (cmd == "wait") {
            headless::run_for(std::atoi(arg(1).c_str()));
        } else if (cmd == "key") {
            std::string utf8;
            const uint32_t code = key_code(arg(1), utf8);
            if (!code && utf8.empty()) fail(run, line, "unknown key " + arg(1));
            else press(code, utf8);
        } else if (cmd == "stale") {
            /* a key typed while the app was frozen: press and release queued, LVGL not run (the show must drop them) */
            std::string utf8;
            const uint32_t code = key_code(arg(1), utf8);
            if (!code && utf8.empty()) {
                fail(run, line, "unknown key " + arg(1));
            } else {
                cp0_keyboard_inject_key(code, KBD_KEY_PRESSED, 0, utf8.empty() ? nullptr : utf8.c_str());
                cp0_keyboard_inject_key(code, KBD_KEY_RELEASED, 0, utf8.empty() ? nullptr : utf8.c_str());
            }
        } else if (cmd == "hide" || cmd == "show" || cmd == "close") {
            /* the launcher's message; hide and show must be answered within 300 ms, close must make the app quit */
            const int ms = headless::lifecycle_send(cmd, 300);
            if (ms < 0) fail(run, line, cmd + (cmd == "close" ? ": the app did not quit" : ": no answer") + " within 300 ms");
            else run.report << cmd << " answered in " << ms << " ms\n";
        } else if (cmd == "type") {
            const std::string text = arg(1);
            for (size_t i = 0; i < text.size();) {
                size_t n = 1;
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (c >= 0xF0) n = 4;
                else if (c >= 0xE0) n = 3;
                else if (c >= 0xC0) n = 2;
                std::string utf8;
                const std::string ch = text.substr(i, n);
                uint32_t code = n == 1 ? key_code(ch == " " ? "space" : ch, utf8) : 0;
                press(code, ch);
                i += n;
            }
        } else if (cmd == "tap" || cmd == "tapkey") {
            /* CLOSE: the keyboard's close cross (a symbol, drawn while the writing zone is the first input) */
            lv_obj_t *label = headless::find_label(arg(1) == "CLOSE" ? std::string(LV_SYMBOL_CLOSE) : arg(1),
                                                   cmd == "tapkey");
            if (!label) {
                fail(run, line, "nothing labelled \"" + arg(1) + "\" to tap");
                continue;
            }
            const lv_area_t a = headless::visible_area(headless::clickable_of(label));
            tap_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        } else if (cmd == "tapxy") {
            tap_at(static_cast<int>(std::atof(arg(1).c_str()) * (run.size.w - 1)),
                   static_cast<int>(std::atof(arg(2).c_str()) * (run.size.h - 1)));
        } else if (cmd == "drag") {
            const int x1 = static_cast<int>(std::atof(arg(1).c_str()) * (run.size.w - 1));
            const int y1 = static_cast<int>(std::atof(arg(2).c_str()) * (run.size.h - 1));
            const int x2 = static_cast<int>(std::atof(arg(3).c_str()) * (run.size.w - 1));
            const int y2 = static_cast<int>(std::atof(arg(4).c_str()) * (run.size.h - 1));
            headless::platform_touch(true, x1, y1);
            headless::run_for(40);
            for (int i = 1; i <= 10; ++i) {
                headless::platform_touch(true, x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10);
                headless::run_for(25);
            }
            headless::platform_touch(false, x2, y2);
            headless::run_for(400);
        } else if (cmd == "osk") {
            run.vkb = arg(1);
            write_input_state(run);
        } else if (cmd == "keyboards") {
            run.keyboards = std::atoi(arg(1).c_str());
            write_input_state(run);
        } else if (cmd == "hand") {
            /* hand 0|1: Settings > On-screen keyboard Handwriting + keyboard (1) or Auto / On (0), as input.state says */
            if (arg(1) != "0" && arg(1) != "1") {
                fail(run, line, "usage: hand 0|1");
                continue;
            }
            run.hand = std::atoi(arg(1).c_str());
            write_input_state(run);
        } else if (cmd == "zonekey") {
            /* zonekey NAME [hold MS]: a tap (or a press held MS ms) on a key of the band's strip, through the touch path */
            int x = 0, y = 0, kw = 0, kh = 0;
            if ((w.size() != 2 && !(w.size() == 4 && w[2] == "hold")) || arg(1) == "letters" || arg(1) == "digits" ||
                !headless::band_rect(arg(1), &x, &y, &kw, &kh)) {
                fail(run, line, "zonekey Keyb|Tab|Ctrl|Alt|Left|Up|Down|Right|Enter [hold MS] (a band scene, portrait)");
                continue;
            }
            const int held = w.size() == 4 ? std::max(0, std::atoi(arg(3).c_str())) : 80;
            headless::platform_touch(true, x + kw / 2, y + kh / 2);
            headless::run_for(held);
            headless::platform_touch(false, x + kw / 2, y + kh / 2);
            headless::run_for(300);
        } else if (cmd == "write") {
            /* write letters|digits GLYPH ...: each glyph's shipped stroke in that box (a letter, a digit, or a gesture:
             * space enter bksp shift ext) */
            if (w.size() < 3 || (arg(1) != "letters" && arg(1) != "digits")) {
                fail(run, line, "usage: write letters|digits GLYPH ...");
                continue;
            }
            for (size_t i = 2; i < w.size(); ++i)
                if (!headless::band_write(arg(1) == "digits", w[i]))
                    fail(run, line, "write: no band, or no glyph \"" + w[i] + "\" in " + arg(1));
        } else if (cmd == "stroke") {
            /* stroke letters|digits X,Y ...: a pen stroke, the points in px from the box's top-left corner */
            int bx = 0, by = 0, bw = 0, bh = 0;
            if (w.size() < 3 || (arg(1) != "letters" && arg(1) != "digits") ||
                !headless::band_rect(arg(1), &bx, &by, &bw, &bh)) {
                fail(run, line, "usage: stroke letters|digits X,Y ... (a band scene, portrait)");
                continue;
            }
            std::vector<std::pair<int, int>> pts;
            for (size_t i = 2; i < w.size(); ++i) {
                int x = 0, y = 0;
                if (std::sscanf(w[i].c_str(), "%d,%d", &x, &y) == 2) pts.emplace_back(bx + x, by + y);
                else fail(run, line, "stroke: bad point " + w[i]);
            }
            headless::band_stroke(pts);
        } else if (cmd == "shot") {
            shot(run, line, arg(1), true);
        } else if (cmd == "check") {
            shot(run, line, arg(1), false);
        } else if (cmd == "expect") {
            const std::string what = arg(1);
            if (what == "quit" && !cdk::quit_requested()) fail(run, line, "expected the app to quit");
            else if (what == "noquit" && cdk::quit_requested()) fail(run, line, "the app quit");
            else if (what == "osk") {
                const bool want = arg(2) == "on";
                if (cp0::ui::osk::visible() != want) fail(run, line, "expected the on-screen keyboard " + arg(2));
            } else if (what == "band") {
                /* expect band shown|hidden|none: the writing zone's band (none: this size has no band) */
                const std::string now = !headless::band_on() ? "none" : headless::band_shown() ? "shown" : "hidden";
                if (arg(2) != "shown" && arg(2) != "hidden" && arg(2) != "none")
                    fail(run, line, "expect band shown|hidden|none");
                else if (now != arg(2))
                    fail(run, line, "expected the band " + arg(2) + ", it is " + now);
            } else if (what == "hidden" && !cdk::hidden()) {
                fail(run, line, "expected the app hidden");
            } else if (what == "visible" && cdk::hidden()) {
                fail(run, line, "expected the app visible");
            } else if (what == "nowrite" && headless::output_stats().flushes_while_off != 0) {
                /* the helper turned invalidation off: not one flush while hidden (none would be written anyway) */
                fail(run, line, std::to_string(headless::output_stats().flushes_while_off) + " flush(es) while hidden");
            } else if (what == "repaint" &&
                       headless::output_stats().pixels_since_on < static_cast<long>(run.size.w) * run.size.h) {
                fail(run, line, "the show repainted " + std::to_string(headless::output_stats().pixels_since_on) +
                                    " of " + std::to_string(run.size.w * run.size.h) + " pixels");
            } else if (what == "text" && !headless::find_label(arg(2), false)) {
                fail(run, line, "no visible text \"" + arg(2) + "\"");
            } else if (what == "notext" && headless::find_label(arg(2), false)) {
                fail(run, line, "text \"" + arg(2) + "\" is visible");
            } else if (what == "key") {
                /* expect key "TEXT" toolbar|target: the button labelled TEXT is exactly that high and at least that
                 * wide (toolbar = cp0_ui_toolbar_key_px, the 9 mm toolbar key; target = TARGET) */
                const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
                const bool toolbar = arg(3) == "toolbar";
                const int want = toolbar ? cp0_ui_toolbar_key_px(m->ppmm_x100) : m->tok[CP0_TOK_TARGET];
                lv_obj_t *label = headless::find_label(arg(2), false);
                if (arg(3) != "toolbar" && arg(3) != "target") {
                    fail(run, line, "expect key: toolbar or target, not \"" + arg(3) + "\"");
                } else if (!label) {
                    fail(run, line, "no visible text \"" + arg(2) + "\"");
                } else {
                    lv_obj_t *button = headless::clickable_of(label);
                    const int bw = lv_obj_get_width(button), bh = lv_obj_get_height(button);
                    run.report << "key \"" << arg(2) << "\" " << bw << "x" << bh << " (" << arg(3) << " " << want
                               << ", toolbar " << cp0_ui_toolbar_key_px(m->ppmm_x100) << ", target "
                               << m->tok[CP0_TOK_TARGET] << ")\n";
                    if (bh != want || bw < want)
                        fail(run, line, "key \"" + arg(2) + "\" is " + std::to_string(bw) + "x" + std::to_string(bh) +
                                            ", expected " + std::to_string(want) + " high and at least as wide");
                }
            }
            run.report << "expect " << what << (arg(2).empty() ? "" : " " + arg(2)) << "\n";
        } else if (cmd == "say") {
            std::fprintf(stderr, "[%s] %s\n", run.size.name().c_str(), arg(1).c_str());
        } else {
            fail(run, line, "unknown command " + cmd);
        }
    }
}

int run_size(const Size &size, const Scene &scene, const std::string &out_dir, const std::string &resources)
{
    Run run;
    run.size = size;
    run.out = out_dir + "/" + size.name();
    run.runtime = run.out + "/runtime/applaunch";
    mkdirs(run.runtime);
    run.report.open(run.out + "/report.txt");
    run.report << "scene " << scene.name << " at " << size.name() << "\n";

    setenv("TZ", "UTC", 1);
    tzset();
    unsetenv("APPLAUNCH_OSK");
    unsetenv("APPLAUNCH_PDA"); /* band scenes: band_begin sets it where the size has a band */
    setenv("APPLAUNCH_STATE_DIR", run.runtime.c_str(), 1); /* never the real runtime directory */
    run.hand = scene.band ? 1 : 0; /* the PDA profile's default choice: Handwriting + keyboard */
    if (!scene.nocontract) {
        /* the per-launch environment of a fullscreen app (app_launch_env.cpp in the launcher) */
        setenv("APPLAUNCH_CONTRACT", "1", 1);
        setenv("APPLAUNCH_APP_MODE", "fullscreen", 1);
        setenv("APPLAUNCH_WINDOW", ("0,0," + std::to_string(size.w) + "," + std::to_string(size.h)).c_str(), 1);
        setenv("APPLAUNCH_CANVAS", size.name().c_str(), 1);
        setenv("APPLAUNCH_CANVAS_SCALE", "1", 1);
        write_input_state(run);
    } else {
        for (const char *v : {"APPLAUNCH_CONTRACT", "APPLAUNCH_APP_MODE", "APPLAUNCH_WINDOW", "APPLAUNCH_CANVAS",
                              "APPLAUNCH_CANVAS_SCALE"})
            unsetenv(v);
        std::remove((run.runtime + "/input.state").c_str());
    }

    for (const auto &kv : scene.env) setenv(kv.first.c_str(), kv.second.c_str(), 1);

    if (!headless::platform_init(size.w, size.h, default_ppmm(size), resources)) {
        std::fprintf(stderr, "[%s] platform init failed\n", size.name().c_str());
        return 1;
    }
    if (scene.band) {
        /* a portrait canvas of a launcher with the writing zone: the backend builds the band (none in landscape) */
        if (headless::band_begin(resources + "/strokes")) {
            run.report << "band on (APPLAUNCH_PDA=1): from row " << headless::band_strip_y() << "\n";
        } else if (size.h > size.w) {
            ++run.failures;
            run.report << "FAIL band: no writing zone at this portrait size\n";
        } else {
            run.report << "band: none at this size (landscape: APPLAUNCH_PDA=0, as on a board)\n";
        }
    }
    const cdk::AppSpec &spec = cdk::app_spec();
    run.notice = !cdk::screen_fits(spec, size.w, size.h);
    cdk::start(spec, lv_screen_active());
    if (scene.lifecycle) {
        if (headless::lifecycle_begin()) {
            run.report << "lifecycle on (the harness plays the launcher)\n";
        } else {
            ++run.failures;
            run.report << "FAIL lifecycle: the helper did not start\n";
        }
    }
    headless::run_for(300);
    play(run, scene);
    cdk::stop();
    {
        std::ofstream list(run.out + "/shots.list");
        for (const std::string &s : run.shots) list << s << "\n";
    }
    run.report << (run.failures ? "FAILED " : "PASSED ") << run.failures << " failure(s)\n";
    run.report.close();
    return std::min(run.failures, 100);
}

} // namespace

int main(int argc, char **argv)
{
    std::string scene_path, out_dir = "out", resources, sizes_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--scene") scene_path = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--resources") resources = next();
        else if (a == "--sizes") sizes_arg = next();
        else {
            std::fprintf(stderr, "usage: %s --scene FILE [--sizes WxH,...] [--out DIR] [--resources DIR]\n", argv[0]);
            return 2;
        }
    }
    if (scene_path.empty()) {
        std::fprintf(stderr, "--scene is needed\n");
        return 2;
    }
    if (resources.empty()) {
        const char *env = std::getenv("CDK_HEADLESS_RESOURCES");
        resources = env ? env : "";
    }
    Scene scene;
    if (!load_scene(scene_path, scene)) return 2;
    if (!sizes_arg.empty()) {
        scene.sizes.clear();
        std::stringstream list(sizes_arg);
        std::string item;
        while (std::getline(list, item, ',')) {
            Size s;
            if (parse_size(item, s)) scene.sizes.push_back(s);
        }
    }
    if (scene.sizes.empty())
        for (const char *s : {"640x480", "800x480", "720x720", "1280x720", "1024x600", "480x320"}) {
            Size size;
            parse_size(s, size);
            scene.sizes.push_back(size);
        }
    const std::string scene_out = out_dir + "/" + scene.name;
    mkdirs(scene_out);

    std::vector<std::pair<pid_t, Size>> children;
    for (const Size &size : scene.sizes) {
        std::fflush(nullptr);
        const pid_t pid = fork();
        if (pid == 0) {
            const std::string log = scene_out + "/" + size.name() + ".log";
            mkdirs(scene_out + "/" + size.name());
            if (FILE *f = std::freopen(log.c_str(), "w", stderr)) (void)f;
            _exit(run_size(size, scene, scene_out, resources));
        }
        if (pid > 0) children.emplace_back(pid, size);
    }
    int failed = 0;
    for (auto &child : children) {
        int status = 0;
        waitpid(child.first, &status, 0);
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 100;
        std::printf("%s %s %s (%s/%s/report.txt)\n", code == 0 ? "PASS" : "FAIL", scene.name.c_str(),
                    child.second.name().c_str(), scene_out.c_str(), child.second.name().c_str());
        if (code != 0) {
            ++failed;
            std::ifstream rep(scene_out + "/" + child.second.name() + "/report.txt");
            std::string l;
            while (std::getline(rep, l))
                if (l.rfind("FAIL", 0) == 0) std::printf("    %s\n", l.c_str());
            if (!WIFEXITED(status)) std::printf("    crashed (signal %d), see %s.log\n", WTERMSIG(status), child.second.name().c_str());
        }
    }
    /* the contact sheet: shots in the order of the first size that has them */
    std::vector<std::string> shots, names;
    for (const Size &size : scene.sizes) {
        names.push_back(size.name());
        std::ifstream list(scene_out + "/" + size.name() + "/shots.list");
        std::string s;
        while (std::getline(list, s))
            if (!s.empty() && std::find(shots.begin(), shots.end(), s) == shots.end()) shots.push_back(s);
    }
    const std::string sheet = out_dir + "/sheets/" + scene.name + ".png";
    mkdirs(out_dir + "/sheets");
    if (!shots.empty() &&
        headless::write_sheet(sheet, scene.title, names, shots, scene_out, resources + "/share/font/Montserrat-Medium.ttf"))
        std::printf("sheet %s\n", sheet.c_str());
    return failed ? 1 : 0;
}
