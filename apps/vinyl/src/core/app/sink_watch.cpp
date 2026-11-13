/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl: following the sound server's sinks (see sink_watch.hpp).
 */
#include "sink_watch.hpp"

#include "pulse_worker.hpp"

namespace vinyl {

PulseChangeCallback SinkWatch::callback() const {
    if (installed_) return PulseChangeCallback();
    std::shared_ptr<std::atomic<bool>> flag = flag_;
    return [flag](const PulseChange &ch) {
        /* a sink's volume or mute, a sink coming or going (a reconnect), the default sink, a card's profile; the loss of
         * the connection (the next scan connects again) */
        if (ch.what == PulseChange::What::Sink || ch.what == PulseChange::What::Server || ch.what == PulseChange::What::Card)
            flag->store(true);
    };
}

void SinkWatch::scanned(const ScanResult &r) {
    if (r.callback_set) installed_ = true;
    else if (!r.connected || r.connect_result == 0) installed_ = false; /* a new connection needs the callback again */
}

}  // namespace vinyl
