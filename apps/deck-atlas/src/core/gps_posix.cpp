/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the real serial-port layer of the GPS reader (see gps_posix.hpp).
 */
#include "gps_posix.hpp"

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

namespace atlas {

namespace {

std::string slurp_line(const std::string &path)
{
    std::string s;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[256];
        if (std::fgets(buf, sizeof buf, f)) s = buf;
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
        while (const dirent *e = ::readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        ::closedir(d);
    }
    return out;
}

/* ttyACM2 before ttyACM10: the prefix, then the number */
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

speed_t speed_of(int baud)
{
    switch (baud) {
    case 4800: return B4800;
    case 9600: return B9600;
    case 38400: return B38400;
    case 115200: return B115200;
    default: return B0;
    }
}

} // namespace

bool gps_baud_supported(int baud) { return speed_of(baud) != B0; }

PosixGpsPorts::~PosixGpsPorts()
{
    for (const auto &kv : saved_) ::close(kv.first);
}

std::vector<PortInfo> PosixGpsPorts::list_ports()
{
    std::vector<std::string> names;
    for (const std::string &n : list_dir(roots_.dev))
        if (is_candidate(n)) names.push_back(n);
    std::sort(names.begin(), names.end(), tty_less);

    std::vector<std::pair<std::string, std::string>> by_id; /* link name -> tty */
    const std::string by_id_dir = roots_.dev + "/serial/by-id";
    for (const std::string &n : list_dir(by_id_dir)) by_id.emplace_back(n, base_name(real_path(by_id_dir + "/" + n)));

    std::vector<PortInfo> out;
    for (const std::string &tty : names) {
        const std::string node = roots_.dev + "/" + tty;
        struct stat st;
        if (::stat(node.c_str(), &st) != 0) continue;
        PortInfo p;
        p.tty = tty;
        p.path = node;
        for (const auto &b : by_id)
            if (b.second == tty) p.by_id = b.first;
        /* the USB device: up from /sys/class/tty/<tty>/device to the first folder with idVendor */
        std::string dir = real_path(roots_.sys + "/class/tty/" + tty + "/device");
        for (int depth = 0; depth < 8 && !dir.empty() && dir != "/"; ++depth) {
            const std::string vid = slurp_line(dir + "/idVendor");
            if (!vid.empty()) {
                p.vid = static_cast<std::uint16_t>(std::strtoul(vid.c_str(), nullptr, 16));
                p.pid = static_cast<std::uint16_t>(std::strtoul(slurp_line(dir + "/idProduct").c_str(), nullptr, 16));
                p.product = slurp_line(dir + "/product");
                break;
            }
            dir = dir.substr(0, dir.rfind('/'));
        }
        out.push_back(p);
    }
    for (const std::string &path : extra_) {
        PortInfo p;
        p.path = path;
        p.tty = base_name(path);
        out.push_back(p);
    }
    return out;
}

int PosixGpsPorts::open(const std::string &path, PortErr &err)
{
    err = PortErr::None;
    /* read-only: the kernel refuses any send on this descriptor */
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        err = e == EBUSY ? PortErr::Busy
              : (e == EACCES || e == EPERM) ? PortErr::Denied
              : (e == ENOENT || e == ENODEV || e == ENXIO) ? PortErr::Gone
                                                           : PortErr::Other;
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
        ::close(fd);
        err = PortErr::Busy;
        return -1;
    }
    ::ioctl(fd, TIOCEXCL); /* best effort: keeps a second opener out while we listen */
    struct termios t;
    if (::tcgetattr(fd, &t) != 0) {
        ::ioctl(fd, TIOCNXCL);
        ::flock(fd, LOCK_UN);
        ::close(fd);
        err = PortErr::Other; /* not a tty */
        return -1;
    }
    saved_[fd] = std::string(reinterpret_cast<const char *>(&t), sizeof t);
    return fd;
}

bool PosixGpsPorts::configure(int handle, int baud)
{
    const speed_t sp = speed_of(baud);
    if (sp == B0) return false;
    struct termios t;
    if (::tcgetattr(handle, &t) != 0) return false;
    ::cfmakeraw(&t);                 /* 8N1, no processing */
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
    t.c_cc[VMIN] = 0;                /* read() returns at once, with what there is */
    t.c_cc[VTIME] = 0;
    ::cfsetispeed(&t, sp);
    ::cfsetospeed(&t, sp);
    return ::tcsetattr(handle, TCSANOW, &t) == 0;
}

void PosixGpsPorts::discard_input(int handle) { ::tcflush(handle, TCIFLUSH); }

long PosixGpsPorts::read(int handle, std::uint8_t *buf, std::size_t n)
{
    const ssize_t r = ::read(handle, buf, n);
    if (r > 0) return r;
    if (r == 0) {
        /* VMIN = 0: nothing available. A hung-up tty (the adapter pulled out) also reads 0 for ever, but its ioctls
         * fail: ask once per empty read. */
        struct termios t;
        if (::tcgetattr(handle, &t) != 0 && (errno == EIO || errno == ENOTTY || errno == ENODEV || errno == ENXIO)) return -1;
        return 0;
    }
    if (errno == EAGAIN || errno == EINTR) return 0;
    return -1;
}

void PosixGpsPorts::close(int handle)
{
    const auto it = saved_.find(handle);
    if (it != saved_.end()) {
        struct termios t;
        if (it->second.size() == sizeof t) {
            std::memcpy(&t, it->second.data(), sizeof t);
            (void)::tcsetattr(handle, TCSANOW, &t); /* a failure is ignored: the port may be gone */
        }
        saved_.erase(it);
    }
    ::ioctl(handle, TIOCNXCL);
    ::flock(handle, LOCK_UN);
    ::close(handle);
}

} // namespace atlas
