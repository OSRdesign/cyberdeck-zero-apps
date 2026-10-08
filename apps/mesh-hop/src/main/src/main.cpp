/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop: full-screen MeshCore messenger (the launcher starts it with X-Fullscreen=true).
 *
 *   M5CardputerZero-mesh-hop                         on the deck: /dev/fb0, the touch screen and the Bluetooth keyboard
 *   mesh-hop --headless [--script f] [--shot-dir d]  no panel, no input device: renders into memory and runs a script
 *
 * Script (one command per line, # starts a comment):
 *   wait <ms>   key <name>   type <text>   tap <x> <y>   hold <x> <y> <ms> [shot]   drag <x1> <y1> <x2> <y2>   bench <key> <n>   shot <name>   dump   say <text>
 *   key names: esc enter tab backtab up down left right pgup pgdn home end bksp del, ctrl-<letter>
 * The screenshots are PNG files in the --shot-dir; `dump` prints one line with the state of the app.
 */

#include "app.hpp"
#include "platform.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace meshhop;

namespace {

std::atomic<bool> g_quit{false};

void on_signal(int)
{
    g_quit.store(true);          // the launcher ends the app with SIGTERM after a 3 s hold of Esc
}

void run_for(Platform &plat, App &app, uint64_t ms)
{
    const uint64_t end = mono_ms() + ms;
    do {
        plat.poll_input();
        app.tick();
        const uint32_t d = lv_timer_handler();
        plat.wait(static_cast<int>(d < 15 ? d : 15));
    } while (mono_ms() < end && !g_quit.load());
}

bool key_by_name(const std::string &name, KeyEvent &e)
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

int run_script(Platform &plat, App &app, const std::string &file, const std::string &shot_dir)
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
        if (cmd == "wait") {
            int ms = 0;
            ss >> ms;
            run_for(plat, app, static_cast<uint64_t>(ms));
        } else if (cmd == "key") {
            std::string name;
            ss >> name;
            KeyEvent e;
            if (!key_by_name(name, e)) {
                std::fprintf(stderr, "script:%d: unknown key %s\n", lineno, name.c_str());
                ++failures;
            } else {
                plat.inject_key(e);
            }
            run_for(plat, app, 60);
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
                KeyEvent e;
                e.key = Key::Char;
                e.text = text.substr(i, n);
                plat.inject_key(e);
                i += n;
            }
            run_for(plat, app, 60);
        } else if (cmd == "tap") {
            int x = 0, y = 0;
            ss >> x >> y;
            plat.inject_touch(true, x, y);
            run_for(plat, app, 80);
            plat.inject_touch(false, x, y);
            run_for(plat, app, 80);
        } else if (cmd == "hold") {
            // hold <x> <y> <ms> [shot-name]: a finger kept down; with a name, a screenshot is taken at the end of the hold (before the release)
            int x = 0, y = 0, ms = 0;
            std::string shot;
            ss >> x >> y >> ms >> shot;
            plat.inject_touch(true, x, y);
            if (shot.empty()) {
                run_for(plat, app, static_cast<uint64_t>(ms));
            } else {
                run_for(plat, app, static_cast<uint64_t>(ms));
                const std::string path = (shot_dir.empty() ? std::string(".") : shot_dir) + "/" + shot + ".png";
                if (!plat.screenshot(path)) ++failures;
                else std::printf("shot %s\n", path.c_str());
            }
            plat.inject_touch(false, x, y);
            run_for(plat, app, 150);
        } else if (cmd == "drag") {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            ss >> x1 >> y1 >> x2 >> y2;
            plat.inject_touch(true, x1, y1);
            run_for(plat, app, 60);
            for (int i = 1; i <= 8; ++i) {
                plat.inject_touch(true, x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8);
                run_for(plat, app, 30);
            }
            plat.inject_touch(false, x2, y2);
            run_for(plat, app, 100);
        } else if (cmd == "bench") {
            // bench <key> <n>: n key presses, each followed by one pass of the main loop (no sleeping): the milliseconds per press
            std::string name;
            int n = 0;
            ss >> name >> n;
            KeyEvent e;
            if (!key_by_name(name, e) || n <= 0) {
                std::fprintf(stderr, "script:%d: bench <key> <n>\n", lineno);
                ++failures;
            } else {
                const uint64_t t0 = mono_ms();
                for (int i = 0; i < n; ++i) {
                    plat.inject_key(e);
                    app.tick();
                    lv_timer_handler();
                }
                const uint64_t dt = mono_ms() - t0;
                std::printf("bench %s x%d: %llu ms, %.2f ms per key\n", name.c_str(), n, static_cast<unsigned long long>(dt), static_cast<double>(dt) / n);
                run_for(plat, app, 100);
            }
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            run_for(plat, app, 120);                    // let the last changes be drawn
            const std::string path = (shot_dir.empty() ? std::string(".") : shot_dir) + "/" + name + ".png";
            if (!plat.screenshot(path)) {
                std::fprintf(stderr, "script:%d: cannot write %s\n", lineno, path.c_str());
                ++failures;
            } else {
                std::printf("shot %s\n", path.c_str());
            }
        } else if (cmd == "dump") {
            run_for(plat, app, 60);
            std::printf("%s\n", app.debug_state().c_str());
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
        } else {
            std::fprintf(stderr, "script:%d: unknown command %s\n", lineno, cmd.c_str());
            ++failures;
        }
        std::fflush(stdout);
    }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
    bool headless = false;
    std::string script, shot_dir;
    if (const char *e = std::getenv("MESHHOP_HEADLESS"); e && *e && *e != '0') headless = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") headless = true;
        else if (a == "--script" && i + 1 < argc) script = argv[++i];
        else if (a == "--shot-dir" && i + 1 < argc) shot_dir = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--headless] [--script file] [--shot-dir dir]\n", argv[0]);
            return a == "--help" ? 0 : 2;
        }
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    Platform plat;
    if (!plat.init(headless)) return 1;
    int rc = 0;
    {
        App app(plat);
        if (!script.empty()) {
            rc = run_script(plat, app, script, shot_dir);
        } else {
            while (!g_quit.load()) run_for(plat, app, 10);
        }
    }
    return rc;
}
