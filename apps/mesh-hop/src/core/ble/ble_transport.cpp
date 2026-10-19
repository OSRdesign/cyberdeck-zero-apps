/*
 * SPDX-License-Identifier: MIT
 */

#include "ble_transport.hpp"

#include <chrono>
#include <cstring>

namespace meshzero::ble {

BleTransport::BleTransport(std::unique_ptr<IBleBackend> backend, std::string address, std::string name, Clock clock,
                           BleTimings timings)
    : backend_(std::move(backend)), link_(*backend_, timings), clock_(std::move(clock)), address_(std::move(address))
{
    info_.path = "ble:" + address_;
    info_.product = std::move(name);
    info_.board_guess = "MeshCore companion over Bluetooth LE";
}

BleTransport::~BleTransport()
{
    shutdown();
}

uint64_t BleTransport::now() const
{
    if (clock_) return clock_();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void BleTransport::poll()
{
    if (!started_) return;
    link_.tick(now());
}

std::string BleTransport::state_text() const
{
    const BleStatus &s = link_.status();
    const std::string who = s.name.empty() ? address_ : s.name;
    switch (s.state) {
    case BleState::Idle: return who + ": not connected";
    case BleState::Scanning: return who + ": looking for the board";
    case BleState::Pairing: return who + (s.pin_needed ? ": type the board's PIN" : ": pairing");
    case BleState::Connecting: return who + ": " + (s.detail.empty() ? "connecting" : s.detail);
    case BleState::Reconnecting: return who + ": " + s.detail + ", trying again";
    case BleState::Connected: return who + ": connected";
    case BleState::Failed: return who + ": " + fail_text(s.fail) + (s.detail.empty() ? "" : " (" + s.detail + ")");
    }
    return "";
}

OpenStatus BleTransport::open()
{
    if (!started_) {
        started_ = true;
        link_.connect(address_, now());
    }
    poll();
    const BleStatus &s = link_.status();
    if (s.state == BleState::Connected) {
        open_ = true;
        open_gen_ = link_.generation();
        rx_.clear();
        rx_off_ = 0;
        unframer_.reset();
        if (!s.name.empty()) info_.product = s.name;
        error_.clear();
        return OpenStatus::Ok;
    }
    error_ = state_text();
    if (s.state == BleState::Failed) return OpenStatus::Error;
    return OpenStatus::NotFound;
}

void BleTransport::close()
{
    open_ = false;
    rx_.clear();
    rx_off_ = 0;
    unframer_.reset();
}

void BleTransport::shutdown()
{
    close();
    if (started_) link_.disconnect();
    started_ = false;
    if (backend_ && backend_->started()) backend_->stop();
}

long BleTransport::read(uint8_t *buf, size_t max)
{
    if (!open_) return -1;
    poll();
    if (link_.generation() != open_gen_ || !link_.connected()) {
        error_ = "Bluetooth link lost (" + link_.status().detail + ")";
        return -1;
    }
    Bytes frame;
    while (link_.receive(frame)) {
        const Bytes w = wrap_from_device(frame);
        rx_.insert(rx_.end(), w.begin(), w.end());
    }
    const size_t avail = rx_.size() - rx_off_;
    if (avail == 0 || max == 0) return 0;
    const size_t n = avail < max ? avail : max;
    std::memcpy(buf, rx_.data() + rx_off_, n);
    rx_off_ += n;
    if (rx_off_ == rx_.size()) {
        rx_.clear();
        rx_off_ = 0;
    }
    return static_cast<long>(n);
}

bool BleTransport::write(const uint8_t *data, size_t len)
{
    if (!open_ || link_.generation() != open_gen_ || !link_.connected()) return false;
    std::vector<Bytes> frames;
    unframer_.feed(data, len, frames);
    for (const Bytes &f : frames)
        if (!link_.send(f)) return false;
    return true;
}

} // namespace meshzero::ble
