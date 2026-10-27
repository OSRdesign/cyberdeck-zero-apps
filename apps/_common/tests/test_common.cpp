/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of _common: the on-screen keyboard policy (input.state re-read rules, the no-file fallback), the
 * background command runner (lines in order on the LVGL thread, exit codes, timeout, cancel, the process group,
 * shutdown), the minimum-size rule and the multitasking lifecycle (off without the launcher's socket; hide / show
 * answered with one). Built with LVGL (async_run delivers through lv_async_call) on a small headless display. Run by
 * ../build_app.sh host.
 */
#include "app.hpp"
#include "async_run.hpp"
#include "cp0_app_lifecycle_lvgl.h"
#include "headless.hpp"
#include "osk_policy.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (c) ++g_pass;                                                                                               \
        else {                                                                                                         \
            ++g_fail;                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                 \
        }                                                                                                              \
    } while (0)

namespace {

std::string g_dir;

/* Gone or a zombie (an orphan waiting for init to reap it is no longer running). */
bool process_running(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    const size_t paren = line.rfind(')');
    return paren != std::string::npos && paren + 2 < line.size() && line[paren + 2] != 'Z';
}

void write_input(const std::string &path, unsigned keyboards, const char *vkb, unsigned gen, long kb_gone = 0,
                 long bt_gone = 0)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        f << "v=1\nkeyboards=" << keyboards << "\nlast_seen=0\nbt_keyboards=0\nkb_gone=" << kb_gone
          << "\nbt_gone=" << bt_gone << "\nvkb=" << vkb << "\ngen=" << gen << "\n";
    }
    std::rename(tmp.c_str(), path.c_str());
}

void test_osk_fallback()
{
    using cdk::osk_policy::fallback_allows;
    CHECK(fallback_allows("force", 3));
    CHECK(!fallback_allows("off", 0));
    CHECK(fallback_allows("", 0));      /* auto, no keyboard */
    CHECK(!fallback_allows("", 1));     /* auto, a keyboard */
    CHECK(fallback_allows("auto", -1)); /* unknown count: no keyboard assumed */
    CHECK(fallback_allows("bogus", 0)); /* an unknown value is auto */

    int keyboards = 1;
    cdk::osk_policy::Policy p(g_dir + "/missing.state", "", [&] { return keyboards; });
    CHECK(!p.allowed(0));
    CHECK(!p.from_file());
    keyboards = 0;
    CHECK(!p.allowed(500)); /* not looked at again within the second */
    CHECK(p.allowed(1000));
    CHECK(p.looks() == 2);
}

void test_osk_file()
{
    const std::string path = g_dir + "/input.state";
    write_input(path, 0, "auto", 1);
    cdk::osk_policy::Policy p(path, "off", [] { return 5; }); /* the file wins over the environment */
    const std::int64_t t0 = 1000000;                          /* ms, a monotonic time far from 0 */
    CHECK(p.allowed(t0));
    CHECK(p.from_file() && p.gen() == 1 && p.parses() == 1);
    CHECK(p.allowed(t0 + 10));
    CHECK(p.parses() == 1 && p.looks() == 1); /* at most one look a second */

    write_input(path, 1, "auto", 2); /* a keyboard connects */
    CHECK(p.allowed(t0 + 500));      /* not seen yet */
    CHECK(!p.allowed(t0 + 1000));    /* the next look takes gen 2 */
    CHECK(p.gen() == 2 && p.parses() == 2);
    CHECK(!p.allowed(t0 + 2100));
    CHECK(p.parses() == 2 && p.looks() == 3); /* unchanged file: looked at, not parsed */

    /* same gen, other contents: ignored (the launcher always raises gen) */
    write_input(path, 0, "auto", 2);
    CHECK(!p.allowed(t0 + 3200));
    CHECK(p.parses() == 3 && p.input().keyboards == 1);

    /* the keyboard went away 1 s ago: the 2 s grace; 3 s later the keyboard may show */
    const long now_s = static_cast<long>((t0 + 4300) / 1000);
    write_input(path, 0, "auto", 3, now_s - 1);
    CHECK(!p.allowed(t0 + 4300));
    CHECK(p.allowed(t0 + 6400));
    /* a Bluetooth keyboard went to sleep 100 s ago: still counted as present (600 s) */
    write_input(path, 0, "auto", 4, now_s - 100, now_s - 100);
    CHECK(!p.allowed(t0 + 7500));
    CHECK(p.allowed(t0 + 7500 + 600000));
    write_input(path, 2, "force", 5);
    CHECK(p.allowed(t0 + 700000));
    write_input(path, 0, "off", 6);
    CHECK(!p.allowed(t0 + 701000));
    /* the file goes: the fallback (environment "off") */
    std::remove(path.c_str());
    CHECK(!p.allowed(t0 + 702000));
    CHECK(!p.from_file());
}

void test_split_lines()
{
    std::vector<std::string> out;
    std::string buf = "one\ntwo\r\nthr";
    cdk::async_run::split_lines(buf, 100, out);
    CHECK(out.size() == 2 && out[0] == "one" && out[1] == "two" && buf == "thr");
    buf += "ee\n\n";
    out.clear();
    cdk::async_run::split_lines(buf, 100, out);
    CHECK(out.size() == 2 && out[0] == "three" && out[1].empty() && buf.empty());
    buf = std::string(10, 'x') + "\n" + std::string(7, 'y');
    out.clear();
    cdk::async_run::split_lines(buf, 4, out);
    CHECK(out.size() == 4 && out[0] == "xxxx" && out[2] == "xx" && out[3] == "yyyy" && buf == "yyy");
}

struct Outcome {
    std::vector<std::string> lines;
    bool done = false;
    cdk::async_run::Result result;
};

cdk::async_run::JobId start(Outcome &o, std::vector<std::string> argv, int timeout_ms = 0)
{
    cdk::async_run::Spec spec;
    spec.argv = std::move(argv);
    spec.timeout_ms = timeout_ms;
    spec.grace_ms = 300;
    spec.on_line = [&o](const std::string &line) { o.lines.push_back(line); };
    spec.on_done = [&o](const cdk::async_run::Result &r) {
        o.result = r;
        o.done = true;
    };
    return cdk::async_run::run(std::move(spec));
}

bool wait_done(Outcome &o, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!o.done && std::chrono::steady_clock::now() < end) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return o.done;
}

void test_async_run()
{
    {
        Outcome o;
        CHECK(start(o, {"sh", "-c", "echo one; echo two; printf three; exit 3"}) > 0);
        CHECK(wait_done(o, 3000));
        CHECK(o.lines.size() == 3 && o.lines[0] == "one" && o.lines[2] == "three");
        CHECK(o.result.started && o.result.exit_code == 3 && !o.result.ok() && o.result.lines == 3);
    }
    {
        Outcome o; /* order of many lines, across many drains */
        start(o, {"sh", "-c", "i=0; while [ $i -lt 500 ]; do echo $i; i=$((i+1)); done"});
        CHECK(wait_done(o, 5000));
        bool ordered = o.lines.size() == 500;
        for (size_t i = 0; ordered && i < o.lines.size(); ++i) ordered = o.lines[i] == std::to_string(i);
        CHECK(ordered);
        CHECK(o.result.ok());
    }
    {
        Outcome o; /* LC_ALL=C for parse-stable output */
        start(o, {"sh", "-c", "echo $LC_ALL"});
        CHECK(wait_done(o, 3000) && o.lines.size() == 1 && o.lines[0] == "C");
    }
    {
        Outcome o; /* exec failure */
        start(o, {"/nonexistent/cdk-command"});
        CHECK(wait_done(o, 3000) && o.result.exit_code == 127 && !o.result.ok());
    }
    {
        Outcome o; /* timeout: SIGTERM to the group */
        const auto t0 = std::chrono::steady_clock::now();
        start(o, {"sh", "-c", "echo start; sleep 10"}, 300);
        CHECK(wait_done(o, 4000));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(o.result.timed_out && !o.result.ok() && o.lines.size() == 1 && ms < 2500);
    }
    {
        Outcome o; /* cancel ends the whole process group, the background child too */
        const auto id = start(o, {"sh", "-c", "sleep 30 & echo $!; wait"});
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (o.lines.empty() && std::chrono::steady_clock::now() < end) {
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(o.lines.size() == 1);
        const pid_t grandchild = o.lines.empty() ? 0 : static_cast<pid_t>(std::atoi(o.lines[0].c_str()));
        cdk::async_run::cancel(id);
        CHECK(wait_done(o, 4000));
        CHECK(o.result.cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(grandchild > 0 && !process_running(grandchild));
    }
    {
        Outcome o; /* the leader exits, a child it left holds the pipe: ended, the job does not hang */
        const auto t0 = std::chrono::steady_clock::now();
        start(o, {"sh", "-c", "sleep 30 & echo left"});
        CHECK(wait_done(o, 4000));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(o.result.exit_code == 0 && ms < 3000);
    }
    {
        Outcome o; /* shutdown: no callback afterwards, the process is gone */
        start(o, {"sh", "-c", "echo $$; sleep 30"});
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (o.lines.empty() && std::chrono::steady_clock::now() < end) {
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const pid_t pid = o.lines.empty() ? 0 : static_cast<pid_t>(std::atoi(o.lines[0].c_str()));
        CHECK(cdk::async_run::running() == 1);
        cdk::async_run::shutdown();
        CHECK(cdk::async_run::running() == 0);
        wait_done(o, 500);
        CHECK(!o.done);
        CHECK(pid > 0 && !process_running(pid));
        Outcome later; /* refused after shutdown */
        CHECK(start(later, {"true"}) == 0);
    }
}

void test_min_size()
{
    cdk::AppSpec spec;
    spec.title = "Template";
    CHECK(cdk::screen_fits(spec, 640, 480));
    CHECK(cdk::screen_fits(spec, 720, 720));
    CHECK(!cdk::screen_fits(spec, 480, 320));
    CHECK(!cdk::screen_fits(spec, 800, 400));
    const std::string notice = cdk::min_size_notice(spec, 480, 320);
    CHECK(notice.find("Template needs a screen of at least 640x480") == 0);
    CHECK(notice.find("This screen is 480x320") != std::string::npos);
    spec.id = "x-app";
    const std::string version = cdk::version_text(spec);
    CHECK(version.find("Template (x-app) ") == 0);
    CHECK(version.find("launcher ") != std::string::npos && version.find("cp0_lvgl ") != std::string::npos);
    CHECK(version.find("needs a screen of at least 640x480\n") != std::string::npos);
    std::printf("%s", version.c_str());

    // landscape only (no X-Portrait): a portrait canvas is held against the landscape minimum, so it never fits
    CHECK(!cdk::has_portrait(spec));
    CHECK(!cdk::screen_fits(spec, 480, 800));
    CHECK(cdk::min_size_notice(spec, 480, 800).find("at least 640x480") != std::string::npos);
    // X-Portrait=480x640: a portrait canvas against it, a landscape or square one against X-MinSize as before
    spec.portrait_w = 480;
    spec.portrait_h = 640;
    CHECK(cdk::has_portrait(spec));
    CHECK(cdk::screen_fits(spec, 480, 640));
    CHECK(cdk::screen_fits(spec, 480, 800));
    CHECK(cdk::screen_fits(spec, 800, 1280));
    CHECK(!cdk::screen_fits(spec, 320, 480));
    CHECK(!cdk::screen_fits(spec, 480, 600));
    CHECK(cdk::screen_fits(spec, 640, 480));
    CHECK(cdk::screen_fits(spec, 720, 720));
    CHECK(!cdk::screen_fits(spec, 480, 320));
    CHECK(!cdk::screen_fits(spec, 600, 480));
    int mw = 0, mh = 0;
    cdk::min_for(spec, 480, 800, &mw, &mh);
    CHECK(mw == 480 && mh == 640);
    cdk::min_for(spec, 800, 480, &mw, &mh);
    CHECK(mw == 640 && mh == 480);
    CHECK(cdk::min_size_notice(spec, 480, 600).find("Template needs a screen of at least 480x640") == 0);
    CHECK(cdk::min_size_notice(spec, 600, 480).find("Template needs a screen of at least 640x480") == 0);
    CHECK(cdk::version_text(spec).find("needs a screen of at least 640x480, in portrait 480x640\n") !=
          std::string::npos);
    spec.portrait_h = 0; /* half a declaration is none */
    CHECK(!cdk::has_portrait(spec));
    CHECK(!cdk::screen_fits(spec, 480, 800));
}

/* Multitasking: without the launcher's control socket nothing is on (the app as before); with one, the toolkit's helper
 * answers hide / show on the LVGL loop and cdk::hidden() follows. LVGL up (platform_init) first. */
void test_lifecycle()
{
    unsetenv("APPLAUNCH_LIFECYCLE");
    unsetenv("APPLAUNCH_CONTROL_FD");
    CHECK(!cdk::hidden());
    CHECK(!cdk::leave_to_home());
    CHECK(cp0_app_lifecycle_lvgl_start(nullptr, nullptr) == 0);
    CHECK(!cp0_app_lifecycle_lvgl_active());
    setenv("APPLAUNCH_LIFECYCLE", "1", 1);
    setenv("APPLAUNCH_CONTROL_FD", "0", 1); /* stdin: not a socket */
    CHECK(cp0_app_lifecycle_lvgl_start(nullptr, nullptr) == 0);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    setenv("APPLAUNCH_CONTROL_FD", std::to_string(sv[1]).c_str(), 1);
    CHECK(cp0_app_lifecycle_lvgl_start(nullptr, nullptr) == 1);
    CHECK(cp0_app_lifecycle_lvgl_active() && !cdk::hidden());
    char buf[32];
    CHECK(send(sv[0], "hide 1", 6, MSG_NOSIGNAL) == 6);
    headless::run_for(20);
    CHECK(cdk::hidden());
    ssize_t r = recv(sv[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    CHECK(r == 5 && std::string(buf, 5) == "ack 1");
    CHECK(send(sv[0], "show 2", 6, MSG_NOSIGNAL) == 6);
    headless::run_for(20);
    CHECK(!cdk::hidden());
    r = recv(sv[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    CHECK(r == 5 && std::string(buf, 5) == "ack 2");
    CHECK(cdk::leave_to_home());
    r = recv(sv[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    CHECK(r == 4 && std::string(buf, 4) == "home");
    cp0_app_lifecycle_lvgl_stop();
    CHECK(!cp0_app_lifecycle_lvgl_active() && !cdk::leave_to_home());
    close(sv[0]);
    unsetenv("APPLAUNCH_LIFECYCLE");
    unsetenv("APPLAUNCH_CONTROL_FD");
}

} // namespace

int main()
{
    char tmpl[] = "/tmp/cdk-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) return 2;
    g_dir = dir;
    test_osk_fallback();
    test_osk_file();
    test_split_lines();
    test_min_size();
    headless::platform_init(320, 240, 0, "/nonexistent"); /* LVGL for lv_async_call */
    test_async_run();
    test_lifecycle();
    std::string cmd = "rm -rf " + g_dir;
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", g_dir.c_str());
    std::printf("test-common: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
