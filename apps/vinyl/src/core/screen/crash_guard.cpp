/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl "Knock-knock": the screen back on a crash (see crash_guard.hpp).
 */
#include "crash_guard.hpp"

#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vinyl {

namespace {

volatile std::sig_atomic_t g_armed = 0;
bool g_installed = false;
char g_path[sizeof(sockaddr_un::sun_path)] = {0};
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
struct sigaction g_old[sizeof kSignals / sizeof kSignals[0]];

void on_crash(int sig) {
    if (g_armed) screen_on_raw(g_path);
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i)
        if (kSignals[i] == sig) sigaction(sig, &g_old[i], nullptr);
    raise(sig); /* delivered with the previous action when the handler returns (a fault: at the faulting instruction again) */
}

}  // namespace

int screen_on_raw(const char *path) {
    if (!path || !path[0]) return -1;
    const size_t n = std::strlen(path);
    if (n >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un a;
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path, n + 1);
    int r = -1;
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0) {
        static const char kLine[] = "screen on\n";
        r = send(fd, kLine, sizeof kLine - 1, MSG_NOSIGNAL | MSG_DONTWAIT) == static_cast<ssize_t>(sizeof kLine - 1) ? 0 : -1;
    }
    close(fd);
    return r;
}

void crash_guard_install(const std::string &socket_path) {
    if (g_installed || socket_path.empty() || socket_path.size() >= sizeof g_path) return;
    std::memcpy(g_path, socket_path.c_str(), socket_path.size() + 1);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_crash;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) sigaction(kSignals[i], &sa, &g_old[i]);
    g_installed = true;
}

void crash_guard_arm(bool armed) { g_armed = armed ? 1 : 0; }

bool crash_guard_armed() { return g_armed != 0; }

}  // namespace vinyl
