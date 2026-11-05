/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: termios2 helpers (see posix_tty.hpp).
 */
#include "posix_tty.hpp"

#include <asm/ioctls.h>
#include <asm/termbits.h>

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

bool tty_restore(int fd, const std::string &blob)
{
    struct termios2 tio;
    if (blob.size() != sizeof(tio)) return false;
    std::memcpy(&tio, blob.data(), sizeof(tio));
    return ioctl(fd, TCSETS2, &tio) == 0;
}

} // namespace mmw
