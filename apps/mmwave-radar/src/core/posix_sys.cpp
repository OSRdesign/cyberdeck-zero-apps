/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the real port layer (see posix_sys.hpp).
 */
#include "posix_sys.hpp"

#include "posix_tty.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mmw {

namespace {

std::string slurp_line(const std::string &path)
{
    std::string s;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), f)) s = buf;
        std::fclose(f);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string real_path(const std::string &p)
{
    char buf[PATH_MAX];
    return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

std::string base_name(const std::string &p)
{
    const std::size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::vector<std::string> list_dir(const std::string &dir)
{
    std::vector<std::string> out;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (const dirent *e = ::readdir(d)) {
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        }
        ::closedir(d);
    }
    return out;
}

/* ttyUSB2 before ttyUSB10: prefix, then the number */
bool tty_less(const std::string &a, const std::string &b)
{
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && !std::isdigit(static_cast<unsigned char>(a[i])) && a[i] == b[i]) ++i;
    const bool da = i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]));
    const bool db = i < b.size() && std::isdigit(static_cast<unsigned char>(b[i]));
    if (da && db) return std::atoi(a.c_str() + i) < std::atoi(b.c_str() + i);
    return a < b;
}

bool is_candidate(const std::string &name)
{
    return name.compare(0, 6, "ttyUSB") == 0 || name.compare(0, 6, "ttyACM") == 0;
}

} // namespace

PosixSys::~PosixSys()
{
    for (const auto &kv : saved_) ::close(kv.first);
}

void PosixSys::add_extra_port(const std::string &path) { extra_.push_back(path); }

std::vector<PortMeta> PosixSys::list_ports()
{
    std::vector<std::string> names;
    for (const std::string &n : list_dir(roots_.dev))
        if (is_candidate(n)) names.push_back(n);
    std::sort(names.begin(), names.end(), tty_less);

    /* /dev/serial/by-id: link name -> tty */
    std::vector<std::pair<std::string, std::string>> by_id;
    const std::string by_id_dir = roots_.dev + "/serial/by-id";
    for (const std::string &n : list_dir(by_id_dir)) by_id.emplace_back(n, base_name(real_path(by_id_dir + "/" + n)));

    std::vector<PortMeta> out;
    for (const std::string &tty : names) {
        const std::string node = roots_.dev + "/" + tty;
        struct stat st;
        if (::stat(node.c_str(), &st) != 0) continue;
        PortMeta m;
        m.tty = tty;
        m.path = node;
        for (const auto &b : by_id)
            if (b.second == tty) m.by_id = b.first;
        /* the USB device: walk up from /sys/class/tty/<tty>/device to the first directory with idVendor */
        std::string dir = real_path(roots_.sys + "/class/tty/" + tty + "/device");
        m.driver = base_name(real_path(roots_.sys + "/class/tty/" + tty + "/device/driver"));
        for (int depth = 0; depth < 8 && !dir.empty() && dir != "/"; ++depth) {
            const std::string vid = slurp_line(dir + "/idVendor");
            if (!vid.empty()) {
                m.vid = static_cast<std::uint16_t>(std::strtoul(vid.c_str(), nullptr, 16));
                m.pid = static_cast<std::uint16_t>(std::strtoul(slurp_line(dir + "/idProduct").c_str(), nullptr, 16));
                m.product = slurp_line(dir + "/product");
                break;
            }
            dir = dir.substr(0, dir.rfind('/'));
        }
        out.push_back(m);
    }
    for (const std::string &p : extra_) {
        PortMeta m;
        m.path = p;
        m.tty = base_name(p);
        out.push_back(m);
    }
    return out;
}

bool PosixSys::held_by_other(const std::string &path)
{
    const std::string target = real_path(path);
    if (target.empty()) return false;
    const pid_t self = ::getpid();
    for (const std::string &pid : list_dir(roots_.proc)) {
        if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) continue;
        if (std::atoi(pid.c_str()) == static_cast<int>(self)) continue;
        const std::string fddir = roots_.proc + "/" + pid + "/fd";
        for (const std::string &fd : list_dir(fddir)) {
            char buf[PATH_MAX];
            const ssize_t n = ::readlink((fddir + "/" + fd).c_str(), buf, sizeof(buf) - 1);
            if (n <= 0) continue;
            buf[n] = '\0';
            if (target == buf) return true;
        }
    }
    return false;
}

int PosixSys::open(const std::string &path, OpenErr &err)
{
    err = OpenErr::None;
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        err = e == EBUSY ? OpenErr::Busy
              : (e == EACCES || e == EPERM) ? OpenErr::Denied
              : (e == ENOENT || e == ENODEV || e == ENXIO) ? OpenErr::Gone
                                                           : OpenErr::Other;
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        ::close(fd);
        err = OpenErr::Busy;
        return -1;
    }
    ::ioctl(fd, TIOCEXCL); /* best effort: keeps a second opener out while we use it */
    std::string blob;
    if (!tty_save(fd, blob)) {
        ::ioctl(fd, TIOCNXCL);
        ::flock(fd, LOCK_UN);
        ::close(fd);
        err = OpenErr::Other; /* not a tty */
        return -1;
    }
    saved_[fd] = blob;
    return fd;
}

bool PosixSys::configure(int handle, int baud) { return tty_set_raw(handle, baud); }

void PosixSys::flush_input(int handle) { ::tcflush(handle, TCIFLUSH); }

long PosixSys::read(int handle, std::uint8_t *buf, std::size_t n)
{
    const ssize_t r = ::read(handle, buf, n);
    if (r > 0) return r;
    if (r == 0) {
        /* VMIN = 0: nothing available. But a hung-up tty (a USB adapter pulled out) also reads 0 forever; its ioctls
         * fail with EIO, so ask once per empty read. */
        struct termios t;
        if (::tcgetattr(handle, &t) != 0 && (errno == EIO || errno == ENOTTY || errno == ENODEV || errno == ENXIO))
            return -1;
        return 0;
    }
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -1; /* EIO, ENODEV, ENXIO: unplugged */
}

void PosixSys::restore(int handle)
{
    const auto it = saved_.find(handle);
    if (it != saved_.end()) tty_restore(handle, it->second);
}

void PosixSys::close(int handle)
{
    saved_.erase(handle);
    ::ioctl(handle, TIOCNXCL); /* give the port back: another app may open it again at once */
    ::flock(handle, LOCK_UN);
    ::close(handle);
}

} // namespace mmw
