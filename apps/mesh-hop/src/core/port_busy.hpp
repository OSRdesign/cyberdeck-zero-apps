/*
 * SPDX-License-Identifier: MIT
 *
 * Is a serial port already open in another process? Looked up before Mesh Hop opens a port (opening raises DTR, which resets
 * ESP32-style boards, and Mesh Hop then writes to it): scan <proc>/<pid>/fd for a link to the tty. Pure and injectable (a fake
 * /proc in the unit tests). A process whose fd directory cannot be read (another user's) is unknown, not busy. Mesh Hop's own
 * process never counts. The repo has no UUCP lock-file convention, so there is none to honour here (flock on the open tty is
 * seen through its fd).
 */

#pragma once

#include <string>

namespace meshzero {

struct PortUser {
    int pid = 0;
    std::string name;            // /proc/<pid>/comm, "" when unreadable
};

/* True and who, when a process other than self_pid holds path open. path may be a /dev/serial/by-id alias (it is resolved);
 * the fd links are compared after resolving them too. self_pid < 0: the calling process. */
bool port_in_use(const std::string &path, PortUser *who = nullptr, const std::string &proc_dir = "/proc", int self_pid = -1);

/* "This port is used by rnsd. Close that program first."; "another program" when the name is unknown. */
std::string port_busy_text(const PortUser &who);

} // namespace meshzero
