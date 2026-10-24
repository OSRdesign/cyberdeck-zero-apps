/*
 * SPDX-License-Identifier: MIT
 *
 * USB serial transport (termios, libc only). Finds the board by itself:
 *   1. $MESHHOP_PORT, or the first line of <data dir>/port, names the port to use (the simulator, a udev alias);
 *   2. else /dev/ttyACM* and /dev/ttyUSB* are listed with their USB ids read from sysfs; with several, the board
 *      used last time wins (<data dir>/lastport holds its vid:pid), then boards with a well known USB id, then
 *      the lowest name.
 * 115200 baud 8N1 raw, CLOCAL, no flow control. The modem lines are not touched (DTR is raised by the open itself); never 1200 baud.
 */

#pragma once

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
/* Picks one candidate (see the file comment); nullptr when the list is empty. last_ids is "vid:pid" or "". */
const SerialCandidate *choose_port(const std::vector<SerialCandidate> &list, const std::string &last_ids);

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
    int fd_ = -1;
    LinkInfo info_;
    std::string error_;
    std::string tx_;
    bool lost_ = false;
    uint64_t last_exists_check_ms_ = 0;
};

} // namespace meshzero
