/*
 * SPDX-License-Identifier: MIT
 *
 * Is the deck clock trustworthy? `timedatectl show -p NTPSynchronized --value` prints "yes" once the clock was set by
 * NTP (no root needed). If timedatectl is missing or fails, NetworkManager connectivity (`nmcli -t -g CONNECTIVITY
 * general`, "full") is used as a weaker hint. The check runs in a worker thread: nothing here blocks the UI.
 */

#pragma once

#include "clock_policy.hpp"

#include <atomic>
#include <thread>

namespace meshzero {

/* Interpret the output of the two commands (pure, unit tested). */
DeckClock parse_ntp_output(const std::string &timedatectl_output, bool command_ok);
DeckClock parse_connectivity_output(const std::string &nmcli_output, bool command_ok);

class DeckClockProbe {
public:
    DeckClockProbe() = default;
    ~DeckClockProbe();
    DeckClockProbe(const DeckClockProbe &) = delete;
    DeckClockProbe &operator=(const DeckClockProbe &) = delete;

    /* Starts a check (ignored while one runs). */
    void refresh();
    /* Unknown while a check runs and when neither tool answered (the policy then treats the deck as offline). */
    DeckClock state() const;
    /* True once the last check finished, whatever it found. */
    bool answered() const { return result_.load() != 0; }

private:
    void run();
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> result_{0};     // 0 running / never run, 1 synced, 2 unsynced, 3 unknown
};

} // namespace meshzero
