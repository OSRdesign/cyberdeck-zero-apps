/*
 * SPDX-License-Identifier: MIT
 *
 * Byte transport between the app and a MeshCore companion radio. The protocol and the client only see this
 * interface, so a Bluetooth LE (BlueZ GATT) transport can be added later without touching them. Every call is
 * non-blocking: the client polls from the UI timer.
 */

#pragma once

#include "util.hpp"

#include <string>

namespace meshzero {

enum class OpenStatus {
    Ok,
    NotFound,           // no candidate port (no board plugged in)
    PermissionDenied,   // the user is not in the dialout group
    Busy,               // another program has the port
    Error,
};

/* What a transport knows about the device it is connected to (shown on the status screen). */
struct LinkInfo {
    std::string path;            // e.g. /dev/ttyACM0
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string vendor;          // USB manufacturer string when the kernel exposes it
    std::string product;
    std::string board_guess;     // from the USB ids, empty when unknown
};

class ITransport {
public:
    virtual ~ITransport() = default;

    /* Tries to open and configure the link. Cheap and non-blocking; call again later after a failure. */
    virtual OpenStatus open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /* Reads what is available. Returns the byte count (0: nothing now) or -1 when the link is lost (unplugged). */
    virtual long read(uint8_t *buf, size_t max) = 0;
    /* Queues bytes for the device. False when the link is closed or the queue is full. */
    virtual bool write(const uint8_t *data, size_t len) = 0;

    virtual std::string kind() const = 0;                  // "USB serial", later "Bluetooth LE"
    virtual const LinkInfo &info() const = 0;
    virtual std::string last_error() const = 0;            // text for the last failed open / lost link
};

} // namespace meshzero
