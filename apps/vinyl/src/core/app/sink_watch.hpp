/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl: following the sound server's sinks between the periodic scans (board test B9, 2026-11-12: "on evolve 65 the vol
 * chip makes more than a second to reflect"). The Vol chip used to show the launcher's status.state, which Vinyl reads at
 * most once a second and the launcher writes after its own work; a Bluetooth headset's own buttons (AVRCP absolute volume)
 * change the sink without the launcher at all. SinkWatch hands the PulseWorker a change callback (pa_context_subscribe:
 * sink, card and server events) with the next scan; the callback only sets a flag (it runs on the server's thread), and
 * the controller's 100 ms tick asks for a new scan when the flag is up. The chip then shows the sink's own volume
 * (outputs.hpp sink_volume_of) about one tick and one list round trip after the key: well under half a second. Without a
 * server (ALSA boards) nothing changes: the chip keeps the launcher's status.state. LVGL-free.
 */
#pragma once

#include "../pulse/pulse_api.hpp"

#include <atomic>
#include <memory>

namespace vinyl {

struct ScanResult;

class SinkWatch {
public:
    SinkWatch() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    /* The callback to pass with a ScanRequest while none is installed (empty once it is). */
    PulseChangeCallback callback() const;
    /* After a scan: notes that the callback is installed (r.callback_set), or lost with the connection. */
    void scanned(const ScanResult &r);
    bool installed() const { return installed_; }
    /* True once per burst of changes since the last call. */
    bool take_change() { return flag_->exchange(false); }
    /* A scan could not be asked for now (one is running): keep the change for the next tick. */
    void keep_change() { flag_->store(true); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
    bool installed_ = false;
};

}  // namespace vinyl
