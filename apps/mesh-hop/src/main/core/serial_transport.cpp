/*
 * SPDX-License-Identifier: MIT
 */

#include "serial_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace meshzero {

namespace {

uint64_t mono_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string read_first_line(const std::string &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')) line.pop_back();
    return line;
}

/* USB ids and strings of the device behind /sys/class/tty/<name>: walk up from the interface to the device dir. */
bool usb_ids(const std::string &sys_class_tty, const std::string &name, SerialCandidate &c)
{
    char real[PATH_MAX];
    if (!::realpath((sys_class_tty + "/" + name + "/device").c_str(), real)) return false;
    std::string dir = real;
    for (int depth = 0; depth < 6 && dir.size() > 1; ++depth) {
        const std::string vid = read_first_line(dir + "/idVendor");
        if (!vid.empty()) {
            c.vid = static_cast<uint16_t>(std::strtoul(vid.c_str(), nullptr, 16));
            c.pid = static_cast<uint16_t>(std::strtoul(read_first_line(dir + "/idProduct").c_str(), nullptr, 16));
            c.vendor = read_first_line(dir + "/manufacturer");
            c.product = read_first_line(dir + "/product");
            return true;
        }
        const size_t slash = dir.rfind('/');
        if (slash == std::string::npos || slash == 0) break;
        dir.resize(slash);
    }
    return false;
}

bool known_vid(uint16_t vid)
{
    switch (vid) {
    case 0x303A: case 0x239A: case 0x1A86: case 0x10C4: case 0x2886: case 0x0403: case 0x2E8A: return true;
    default: return false;
    }
}

} // namespace

std::string guess_board(uint16_t vid, uint16_t pid)
{
    if (vid == 0x2886 && pid == 0x8044) return "Seeed XIAO nRF52840 (the user's board)";
    switch (vid) {
    case 0x303A: return "Espressif ESP32 native USB (Heltec V3, T-Beam S3, T-Deck, ...)";
    case 0x239A: return "Adafruit nRF52 bootloader (RAK4631, T-Echo, ...)";
    case 0x1A86: return "WCH CH340/CH9102 bridge (Heltec V2/V3, T-Beam, ...)";
    case 0x10C4: return "Silicon Labs CP210x bridge (Heltec V2, T-Beam, ...)";
    case 0x2886: return "Seeed (XIAO, Wio Tracker, T1000-E, ...)";
    case 0x0403: return "FTDI bridge";
    case 0x2E8A: return "Raspberry Pi RP2040 board";
    default: return "";
    }
}

std::vector<SerialCandidate> list_serial_ports(const std::string &dev_dir, const std::string &sys_class_tty)
{
    std::vector<SerialCandidate> out;
    DIR *d = ::opendir(dev_dir.c_str());
    if (!d) return out;
    while (const dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("ttyACM", 0) != 0 && name.rfind("ttyUSB", 0) != 0) continue;
        SerialCandidate c;
        c.path = dev_dir + "/" + name;
        usb_ids(sys_class_tty, name, c);
        out.push_back(c);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const SerialCandidate &a, const SerialCandidate &b) { return a.path < b.path; });
    return out;
}

const SerialCandidate *choose_port(const std::vector<SerialCandidate> &list, const std::string &last_ids)
{
    if (list.empty()) return nullptr;
    char ids[16];
    for (const SerialCandidate &c : list) {
        std::snprintf(ids, sizeof(ids), "%04x:%04x", c.vid, c.pid);
        if (!last_ids.empty() && last_ids == ids) return &c;
    }
    for (const SerialCandidate &c : list)
        if (known_vid(c.vid)) return &c;
    return &list.front();
}

SerialTransport::SerialTransport(std::string data_dir, std::string explicit_path)
    : data_dir_(std::move(data_dir)), explicit_path_(std::move(explicit_path))
{
}

SerialTransport::~SerialTransport()
{
    close();
}

std::string SerialTransport::pick_path(LinkInfo &info)
{
    std::string override_path = explicit_path_;
    if (override_path.empty())
        if (const char *e = std::getenv("MESHHOP_PORT"); e && *e) override_path = e;
    if (override_path.empty() && !data_dir_.empty()) override_path = read_first_line(data_dir_ + "/port");
    if (!override_path.empty()) {
        info.path = override_path;
        info.vid = info.pid = 0;
        return override_path;
    }
    const std::vector<SerialCandidate> list = list_serial_ports();
    const std::string last = data_dir_.empty() ? std::string() : read_first_line(data_dir_ + "/lastport");
    const SerialCandidate *c = choose_port(list, last);
    if (!c) return "";
    info.path = c->path;
    info.vid = c->vid;
    info.pid = c->pid;
    info.vendor = c->vendor;
    info.product = c->product;
    info.board_guess = guess_board(c->vid, c->pid);
    return c->path;
}

OpenStatus SerialTransport::open()
{
    close();
    lost_ = false;
    LinkInfo info;
    const std::string path = pick_path(info);
    if (path.empty()) {
        error_ = "no serial port (plug the board into the USB port)";
        return OpenStatus::NotFound;
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        error_ = path + ": " + std::strerror(e);
        if (e == EACCES || e == EPERM) return OpenStatus::PermissionDenied;
        if (e == EBUSY) return OpenStatus::Busy;
        if (e == ENOENT || e == ENODEV || e == ENXIO) return OpenStatus::NotFound;
        return OpenStatus::Error;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        ::close(fd);
        error_ = path + " is in use by another program";
        return OpenStatus::Busy;
    }
    ::ioctl(fd, TIOCEXCL);      // best effort: keeps a second opener out

    termios tio;
    if (::tcgetattr(fd, &tio) != 0) {
        error_ = path + ": not a serial port (" + std::strerror(errno) + ")";
        ::close(fd);
        return OpenStatus::Error;
    }
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, B115200);
    ::cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        error_ = path + ": cannot configure (" + std::strerror(errno) + ")";
        ::close(fd);
        return OpenStatus::Error;
    }
    // No modem line is touched: opening a CDC ACM / USB serial port already raises DTR (what a nRF52 board waits for),
    // and the real board answered with exactly this sequence (raw 115200 8N1, CLOCAL, no flow control) without resetting.
    // Never a 1200 baud "touch" (that one reboots many boards into their bootloader): the speed is fixed here.
    ::tcflush(fd, TCIOFLUSH);

    fd_ = fd;
    info_ = info;
    tx_.clear();
    last_exists_check_ms_ = mono_ms();
    error_.clear();
    if (!data_dir_.empty() && info.vid) {          // remember which board this was
        char ids[16];
        std::snprintf(ids, sizeof(ids), "%04x:%04x\n", info.vid, info.pid);
        if (FILE *f = std::fopen((data_dir_ + "/lastport").c_str(), "wb")) {
            std::fputs(ids, f);
            std::fclose(f);
        }
    }
    return OpenStatus::Ok;
}

void SerialTransport::close()
{
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    fd_ = -1;
    tx_.clear();
}

void SerialTransport::flush_tx()
{
    while (fd_ >= 0 && !tx_.empty()) {
        const ssize_t n = ::write(fd_, tx_.data(), tx_.size());
        if (n > 0) {
            tx_.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        } else {
            lost_ = true;
            error_ = info_.path + ": write failed (" + std::strerror(errno) + ")";
            return;
        }
    }
}

long SerialTransport::read(uint8_t *buf, size_t max)
{
    if (fd_ < 0) return -1;
    flush_tx();
    if (lost_) return -1;
    pollfd p{fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, 0);
    if (r > 0) {
        if (p.revents & POLLIN) {
            const ssize_t n = ::read(fd_, buf, max);
            if (n > 0) return n;
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                error_ = info_.path + ": link lost";
                return -1;
            }
            return 0;
        }
        if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            error_ = info_.path + ": link lost";
            return -1;
        }
    }
    // an unplugged USB serial device removes its node: look for it twice a second
    const uint64_t now = mono_ms();
    if (now - last_exists_check_ms_ >= 500) {
        last_exists_check_ms_ = now;
        struct stat st;
        if (::stat(info_.path.c_str(), &st) != 0) {
            error_ = info_.path + ": the board was unplugged";
            return -1;
        }
    }
    return 0;
}

bool SerialTransport::write(const uint8_t *data, size_t len)
{
    if (fd_ < 0 || lost_) return false;
    if (tx_.size() + len > 8192) return false;
    tx_.append(reinterpret_cast<const char *>(data), len);
    flush_tx();
    return !lost_;
}

} // namespace meshzero
