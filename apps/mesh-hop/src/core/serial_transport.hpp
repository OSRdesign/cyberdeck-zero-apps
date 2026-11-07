/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial transport (termios, libc only). Finds the board by itself:
 *   1. $MESHHOP_PORT, or the first line of <data dir>/port, names the port to use (the simulator, a udev alias);
 *   2. else /dev/ttyACM* and /dev/ttyUSB* are listed with their USB ids read from sysfs and the port is chosen only when
 *      that is unambiguous: exactly one port, or the board used last time (<data dir>/lastport holds its USB identity,
 *      "vid:pid/serial", or "vid:pid" as written by 0.8.2 and before) is exactly one of them. With several ports and no
 *      such match nothing is guessed and nothing is opened (the start page "Choose a node" lists them and the user picks).
 * A port another process holds open is never opened (port_busy.hpp): open() says Busy and who.
 * 115200 baud 8N1 raw, CLOCAL, no flow control. The modem lines are not touched (DTR is raised by the open itself); never 1200 baud.
 */

#pragma once

#include "port_busy.hpp"
#include "transport.hpp"

#include <vector>

namespace meshzero {

struct SerialCandidate {
    std::string path;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string vendor;
    std::string product;
    std::string serial;          // the USB serial number string ("1C:DB:D4:A8:9A:70"), "" when the device has none
    bool busy = false;           // another process holds the port open (set by the caller, see mark_busy)
    std::string busy_by;         // its name (/proc/<pid>/comm), "" when unknown
};

/* Family of boards behind a USB id, or "" when unknown (informational only). */
std::string guess_board(uint16_t vid, uint16_t pid);
/* Lists the serial ports under dev_dir (ttyACM*, ttyUSB*) with ids from sys_class_tty. */
std::vector<SerialCandidate> list_serial_ports(const std::string &dev_dir = "/dev",
                                               const std::string &sys_class_tty = "/sys/class/tty");
/* The device behind one port (a udev alias such as /dev/serial/by-id/... is resolved first): its USB ids and strings from
 * sys_class_tty; no ids for a port that is not a USB device (a pty). path is kept as given. */
SerialCandidate describe_port(const std::string &path, const std::string &sys_class_tty = "/sys/class/tty");
/* Which USB device this is, to tell two boards on the same port apart: "303a:1001/1C:DB:D4:A8:9A:70" (vid:pid/serial),
 * "1a86:7523" for a device without a serial number, "" when the USB ids are unknown. */
std::string usb_identity(const SerialCandidate &c);
/* Picks one candidate only when that is unambiguous (see the file comment): the only port, else the one that matches
 * last_ident ("vid:pid/serial" or "vid:pid", "" when nothing is remembered) exactly once. nullptr for an empty list and
 * when it cannot be told; ask (when given) is then true for "several ports: let the user choose", false for "none". */
const SerialCandidate *choose_port(const std::vector<SerialCandidate> &list, const std::string &last_ident, bool *ask = nullptr);
/* port_in_use for a serial port to be opened: a pseudo terminal (the simulator, the tests) is never busy. */
bool serial_port_busy(const std::string &path, PortUser *who = nullptr, const std::string &proc_dir = "/proc");
/* Fills busy / busy_by of each candidate from proc_dir (port_busy.hpp). */
void mark_busy(std::vector<SerialCandidate> &list, const std::string &proc_dir = "/proc");

class SerialTransport : public ITransport {
public:
    /* explicit_path: use exactly this port (tests, simulator); "" = override / auto-detect. */
    explicit SerialTransport(std::string data_dir = "", std::string explicit_path = "");
    ~SerialTransport() override;

    OpenStatus open() override;
    void close() override;
    bool is_open() const override { return fd_ >= 0; }
    long read(uint8_t *buf, size_t max) override;
    bool write(const uint8_t *data, size_t len) override;
    std::string kind() const override { return "USB serial"; }
    const LinkInfo &info() const override { return info_; }
    std::string last_error() const override { return error_; }

    int baud() const { return 115200; }

private:
    std::string pick_path(LinkInfo &info);
    void flush_tx();

    std::string data_dir_;
    std::string explicit_path_;
    std::string ident_;          // the USB identity of the port chosen by pick_path ("" for a named port)
    int fd_ = -1;
    LinkInfo info_;
    std::string error_;
    std::string tx_;
    bool lost_ = false;
    uint64_t last_exists_check_ms_ = 0;
};

} // namespace meshzero
