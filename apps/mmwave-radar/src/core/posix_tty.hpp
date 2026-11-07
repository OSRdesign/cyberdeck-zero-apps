/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: termios2 helpers (any baud, e.g. 256000, which has no B constant). In a file of its own because
 * <asm/termbits.h> cannot be included together with <termios.h>.
 */
#pragma once

#include <string>

namespace mmw {

/* The port's current settings as an opaque blob (false: not a tty). */
bool tty_save(int fd, std::string &blob);
/* Raw 8N1 at baud with the least change: no processing, CLOCAL | CREAD, no flow control, VMIN = VTIME = 0. */
bool tty_set_raw(int fd, int baud);
/* The current settings as text (iflag/oflag/cflag/lflag in hex, the flags that matter by name, speeds, VMIN/VTIME). */
std::string tty_describe(int fd);
/* Puts the saved settings back. */
bool tty_restore(int fd, const std::string &blob);

} // namespace mmw
