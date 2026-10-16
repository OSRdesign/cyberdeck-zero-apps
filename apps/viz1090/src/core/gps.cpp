/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: the GPS receiver watcher (see gps.hpp).
 */
#include "gps.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace viz {

namespace {

speed_t baud_constant(int baud)
{
    switch (baud) {
    case 4800: return B4800;
    case 38400: return B38400;
    case 115200: return B115200;
    default: return B9600;
    }
}

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

} // namespace

std::string serial_dev_dir()
{
    const char *env = std::getenv("VIZ_DEV_DIR");
    return env && env[0] ? std::string(env) : std::string("/dev");
}

std::vector<std::string> serial_candidates(const std::string &dir)
{
    std::vector<std::string> ports;
    DIR *d = opendir(dir.c_str());
    if (!d) return ports;
    while (dirent *entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (starts_with(name, "ttyACM") || starts_with(name, "ttyUSB")) ports.push_back(name);
    }
    closedir(d);
    std::sort(ports.begin(), ports.end());
    return ports;
}

NmeaResult listen_nmea(const std::string &path, int baud, int ms, const std::atomic<bool> &stop, bool *denied)
{
    NmeaResult result;
    if (denied) *denied = false;
    const int fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (denied) *denied = errno == EACCES || errno == EPERM;
        return result;
    }
    termios tio{};
    if (tcgetattr(fd, &tio) == 0) { /* a real port: its speed; a test file has none */
        cfmakeraw(&tio);
        cfsetispeed(&tio, baud_constant(baud));
        cfsetospeed(&tio, baud_constant(baud));
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    std::string pending;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop && std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        const int ready = poll(&p, 1, 200);
        if (ready > 0 && (p.revents & POLLIN)) {
            char chunk[256];
            const ssize_t got = read(fd, chunk, sizeof(chunk));
            if (got <= 0) break; /* the end of a test file, or the port went away */
            nmea_feed_bytes(pending, chunk, static_cast<std::size_t>(got), result);
        } else if (ready > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            break;
        }
        if (result.fix) break;
    }
    if (!pending.empty()) nmea_feed_line(pending, result); /* a last line without its end of line (test files) */
    close(fd);
    return result;
}

GpsWatcher::GpsWatcher(std::string dev_dir) : dev_dir_(std::move(dev_dir)) {}

GpsWatcher::~GpsWatcher()
{
    stop();
}

void GpsWatcher::start()
{
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void GpsWatcher::stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

GpsSnapshot GpsWatcher::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

void GpsWatcher::publish(const NmeaResult &best, const std::string &port, const std::string &denied)
{
    std::lock_guard<std::mutex> lock(mutex_);
    GpsSnapshot next = snap_;
    if (!best.seen) {
        next.state = GpsState::None;
        next.port.clear();
        next.denied = denied;
    } else {
        next.state = best.fix ? GpsState::Fix : GpsState::Waiting;
        next.port = port;
        next.denied.clear();
        if (best.fix) {
            next.lat = best.lat;
            next.lon = best.lon;
            next.sats = best.sats;
        }
    }
    const bool changed = snap_.seq == 0 || next.state != snap_.state || next.port != snap_.port ||
                         next.denied != snap_.denied ||
                         next.lat != snap_.lat || next.lon != snap_.lon || next.sats != snap_.sats;
    if (changed) {
        next.seq = snap_.seq + 1;
        snap_ = next;
    }
}

void GpsWatcher::run()
{
    int preferred = 9600;
    while (!stop_) {
        NmeaResult best;
        std::string best_port, denied_port;
        for (const std::string &port : serial_candidates(dev_dir_)) {
            if (stop_) break;
            const int bauds[] = {preferred, 9600, 4800, 38400, 115200};
            for (std::size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]) && !stop_; ++i) {
                if (i > 0 && bauds[i] == preferred) continue;
                bool denied = false;
                const NmeaResult r = listen_nmea(dev_dir_ + "/" + port, bauds[i], i == 0 ? 2500 : 1500, stop_, &denied);
                if (denied) { /* no other speed will open it either */
                    if (denied_port.empty()) denied_port = port;
                    break;
                }
                if (r.seen) {
                    preferred = bauds[i];
                    if (!best.seen || r.fix) {
                        best = r;
                        best_port = port;
                    }
                    break;
                }
            }
            if (best.fix) break;
        }
        if (stop_) break;
        publish(best, best_port, denied_port);
        for (int i = 0; i < 40 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace viz
