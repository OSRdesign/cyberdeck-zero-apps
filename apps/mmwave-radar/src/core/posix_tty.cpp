/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: termios2 helpers (see posix_tty.hpp).
 */
#include "posix_tty.hpp"

#include <asm/ioctls.h>
#include <asm/termbits.h>

#include <cstdio>
#include <cstring>

extern "C" int ioctl(int fd, unsigned long request, ...);

namespace mmw {

bool tty_save(int fd, std::string &blob)
{
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) != 0) return false;
    blob.assign(reinterpret_cast<const char *>(&tio), sizeof(tio));
    return true;
}

bool tty_set_raw(int fd, int baud)
{
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) != 0) return false;
    /* cfmakeraw(), spelled out */
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS | CBAUD | (CBAUD << IBSHIFT));
    tio.c_cflag |= CS8 | CLOCAL | CREAD | BOTHER | (BOTHER << IBSHIFT);
    tio.c_ispeed = static_cast<speed_t>(baud);
    tio.c_ospeed = static_cast<speed_t>(baud);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    return ioctl(fd, TCSETS2, &tio) == 0;
}

std::string tty_describe(int fd)
{
    struct termios2 t;
    if (ioctl(fd, TCGETS2, &t) != 0) return "termios unreadable";
    char b[320];
    std::snprintf(b, sizeof(b),
                  "iflag=%04x oflag=%04x cflag=%04x lflag=%04x ispeed=%u ospeed=%u vmin=%d vtime=%d "
                  "[ixon=%d ixoff=%d icrnl=%d inlcr=%d igncr=%d istrip=%d echo=%d icanon=%d isig=%d opost=%d crtscts=%d cs8=%d "
                  "parenb=%d cstopb=%d clocal=%d cread=%d hupcl=%d bother=%d]",
                  static_cast<unsigned>(t.c_iflag), static_cast<unsigned>(t.c_oflag), static_cast<unsigned>(t.c_cflag),
                  static_cast<unsigned>(t.c_lflag), static_cast<unsigned>(t.c_ispeed), static_cast<unsigned>(t.c_ospeed),
                  t.c_cc[VMIN], t.c_cc[VTIME], !!(t.c_iflag & IXON), !!(t.c_iflag & IXOFF), !!(t.c_iflag & ICRNL),
                  !!(t.c_iflag & INLCR), !!(t.c_iflag & IGNCR), !!(t.c_iflag & ISTRIP), !!(t.c_lflag & ECHO),
                  !!(t.c_lflag & ICANON), !!(t.c_lflag & ISIG), !!(t.c_oflag & OPOST), !!(t.c_cflag & CRTSCTS),
                  (t.c_cflag & CSIZE) == CS8, !!(t.c_cflag & PARENB), !!(t.c_cflag & CSTOPB), !!(t.c_cflag & CLOCAL),
                  !!(t.c_cflag & CREAD), !!(t.c_cflag & HUPCL), (t.c_cflag & CBAUD) == BOTHER);
    return b;
}

bool tty_restore(int fd, const std::string &blob)
{
    struct termios2 tio;
    if (blob.size() != sizeof(tio)) return false;
    std::memcpy(&tio, blob.data(), sizeof(tio));
    return ioctl(fd, TCSETS2, &tio) == 0;
}

} // namespace mmw
