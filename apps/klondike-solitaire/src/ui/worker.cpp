/*
 * SPDX-License-Identifier: MIT
 *
 * The solver helper: the app starts itself again as `<exe> --klondike-worker winnable <user> <rules>` or
 * `... deadend <board text> <rules>` through cdk::async_run (a child process in its own process group, killed on a timeout
 * or when the app ends), so the winnable-deal search and the dead-end check never run on the LVGL thread. The child prints
 * one line (core/session.hpp worker_*_reply) and exits.
 *
 * It must work in the device binary and in the headless harness (whose main is not ours): a static initialiser looks at
 * /proc/self/cmdline before main() and, for the helper's arguments, runs the request and _exit()s: LVGL is never started.
 */
#include "session.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

std::vector<std::string> read_cmdline()
{
    std::vector<std::string> args;
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return args;
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string cur;
    for (char c : all) {
        if (c == '\0') {
            args.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) args.push_back(cur);
    return args;
}

struct WorkerHook {
    WorkerHook()
    {
        const std::vector<std::string> a = read_cmdline();
        if (a.size() < 5 || a[1] != "--klondike-worker") return;
        klondike::Rules rules;
        klondike::Rules::parse(a[4], rules);
        std::string reply;
        if (a[2] == "winnable") reply = klondike::worker_winnable_reply(static_cast<std::uint32_t>(std::strtoul(a[3].c_str(), nullptr, 10)), rules);
        else if (a[2] == "deadend") reply = klondike::worker_deadend_reply(a[3], rules);
        else reply = "error";
        std::fputs((reply + "\n").c_str(), stdout);
        std::fflush(stdout);
        _exit(0);
    }
};

const WorkerHook g_hook;

} // namespace
