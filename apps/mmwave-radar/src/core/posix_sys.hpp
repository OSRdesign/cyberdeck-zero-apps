/*
 * SPDX-License-Identifier: MIT
 *
 * mmWave Radar: the real port layer (Linux): sysfs for the USB identity, /proc for "held by another process", flock +
 * TIOCEXCL for the lock, termios2 for the speed. Roots are injectable (tests point them at a fake tree). It never
 * writes to a port.
 */
#pragma once

#include "port_sys.hpp"

#include <map>
#include <string>
#include <vector>

namespace mmw {

struct SysRoots {
    std::string sys = "/sys";
    std::string dev = "/dev";
    std::string proc = "/proc";
};

class PosixSys : public Sys {
public:
    explicit PosixSys(const SysRoots &roots = SysRoots()) : roots_(roots) {}
    ~PosixSys() override;

    std::vector<PortMeta> list_ports() override;
    bool held_by_other(const std::string &path) override;
    int open(const std::string &path, OpenErr &err) override;
    bool configure(int handle, int baud) override;
    void flush_input(int handle) override;
    long read(int handle, std::uint8_t *buf, std::size_t n) override;
    void restore(int handle) override;
    void close(int handle) override;

    /* Test hook: also list this device node (e.g. a pseudo-terminal slave) as a port. */
    void add_extra_port(const std::string &path);

private:
    SysRoots roots_;
    std::map<int, std::string> saved_; /* handle -> original termios2 blob */
    std::vector<std::string> extra_;
};

} // namespace mmw
