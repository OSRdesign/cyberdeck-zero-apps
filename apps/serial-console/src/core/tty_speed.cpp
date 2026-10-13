/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: any baud rate through termios2 (BOTHER), for the rates that have no B constant (74880). In a file of
 * its own: <asm/termbits.h> cannot be included with <termios.h>.
 */
#include <asm/ioctls.h>
#include <asm/termbits.h>

#include <cerrno>

extern "C" int ioctl(int fd, unsigned long request, ...);

namespace sc {
namespace tty2 {

int set_speed(int fd, int baud)
{
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) != 0) return -1;
    tio.c_cflag &= ~(CBAUD | (CBAUD << IBSHIFT));
    tio.c_cflag |= BOTHER | (BOTHER << IBSHIFT);
    tio.c_ispeed = static_cast<speed_t>(baud);
    tio.c_ospeed = static_cast<speed_t>(baud);
    return ioctl(fd, TCSETS2, &tio) == 0 ? 0 : -1;
}

int get_speed(int fd)
{
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) != 0) return -1;
    return static_cast<int>(tio.c_ospeed);
}

} // namespace tty2
} // namespace sc
