/*
 * SPDX-License-Identifier: MIT
 *
 * ITransport over Bluetooth LE: the Client talks to a BLE companion exactly as to the USB one (no change in client.cpp).
 *
 *   open()   starts the link to the address the first time; returns Ok only once the link is Connected (paired, notify
 *            subscribed), NotFound while it is on its way (searching, connecting, pairing, reconnecting) and Error when it
 *            Failed (the user must act: last_error() says why). The Client keeps calling it on its timer.
 *   read()   the board's frames, each wrapped as '>' + u16 length + payload for the Client's FrameParser; -1 once when the
 *            link of this session is lost (the Client then closes and opens again: the link reconnects by itself).
 *   write()  the Client's '<' + u16 length + payload frames, one GATT write each.
 *   close()  ends the Client's session only; the link stays up or keeps reconnecting. shutdown() ends the link.
 *   poll()   advances the link while the Client does not call (the app calls it from its timer before Client::poll; the
 *            calls above poll too).
 */

#pragma once

#include "../transport.hpp"
#include "ble_framing.hpp"
#include "ble_link.hpp"

#include <functional>
#include <memory>

namespace meshzero::ble {

class BleTransport : public ITransport {
public:
    using Clock = std::function<uint64_t()>;   // milliseconds, monotonic

    BleTransport(std::unique_ptr<IBleBackend> backend, std::string address, std::string name = "", Clock clock = {},
                 BleTimings timings = {});
    /* Over a link the app keeps (the start page scans with it and the connection uses it): the transport owns neither the
     * link nor its backend; shutdown() ends the link's connection, the backend stays. */
    BleTransport(BleLink &link, std::string address, std::string name = "", Clock clock = {});
    ~BleTransport() override;

    OpenStatus open() override;
    void close() override;
    bool is_open() const override { return open_; }
    long read(uint8_t *buf, size_t max) override;
    bool write(const uint8_t *data, size_t len) override;
    std::string kind() const override { return "Bluetooth LE"; }
    const LinkInfo &info() const override { return info_; }
    std::string last_error() const override { return error_; }

    void poll() override;
    void shutdown();
    BleLink &link() { return link_; }
    const BleLink &link() const { return link_; }
    const std::string &address() const { return address_; }

private:
    uint64_t now() const;
    std::string state_text() const;

    std::unique_ptr<IBleBackend> backend_;
    std::unique_ptr<BleLink> own_link_;
    BleLink &link_;
    Clock clock_;
    std::string address_;
    LinkInfo info_;
    std::string error_;
    bool started_ = false;
    bool open_ = false;
    uint32_t open_gen_ = 0;
    Bytes rx_;
    size_t rx_off_ = 0;
    ToDeviceUnframer unframer_;
};

} // namespace meshzero::ble
