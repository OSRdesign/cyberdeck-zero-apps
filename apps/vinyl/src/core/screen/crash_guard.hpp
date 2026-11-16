/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl "Knock-knock": the screen back when Vinyl crashes while it is off. The launcher gives the light back by itself
 * about a second after the process dies (APP-CONTRACT section 14, `screen`); this makes it at once on a crash it can catch
 * (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT): the handler sends `screen on` to launcher.sock with async-signal-safe calls
 * only (socket, connect, send, close; non-blocking, no reply awaited), puts the signal's previous action back and raises
 * it again. Armed only while Vinyl may hold the screen off. A clean end (SIGTERM from the launcher's 3 s Esc, SIGINT,
 * SIGHUP, the lifecycle's close, Esc at the root) goes through the app's normal exit instead; SIGKILL cannot be caught
 * (the launcher's own restore covers it). LVGL-free; screen_on_raw is tested in tests/test_knock.cpp.
 */
#pragma once

#include <string>

namespace vinyl {

/* "screen on\n" to the Unix socket at `path`, without waiting for the reply: 0 sent, -1 not (no socket, too long a path,
 * the launcher's queue full). Async-signal-safe. */
int screen_on_raw(const char *path);

/* The handlers, once (the path is copied: the launcher's socket); the real app only, never the tests. */
void crash_guard_install(const std::string &socket_path);
/* Armed while the screen may be off. */
void crash_guard_arm(bool armed);
bool crash_guard_armed();

}  // namespace vinyl
