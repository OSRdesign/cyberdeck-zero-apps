/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette app: the sound-server worker (task T3 of the Bluetooth release). The controller must never hold its mutex across a
 * round trip to the sound server (a hung server answers in up to 3 s per call, T1), so everything that only asks the server a
 * question runs here, on one thread of its own, and the controller takes the answers from its service tick:
 *
 *   scan      (re)connect when asked, then list the sources, sinks and cards (the Bluetooth choosers' data, the connection state);
 *   card      one card by name (the profile poll: connecting, switching to music);
 *   identity  the identity guard of the open capture stream (a silent relink during a take).
 *
 * Each kind has one request in flight at most (a request while one is pending is refused: the caller asks again later) and one
 * finished answer waiting at most (a newer one replaces it). The calls the user triggers themselves (opening a stream) stay on
 * the controller's thread: they wait at most the open timeout (docs/PULSE.md). LVGL-free.
 */
#pragma once

#include "../pulse/pulse_api.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {

struct ScanRequest {
    bool disconnect_first = false; /* the connection was lost: drop it, then connect again */
    bool connect = false;          /* connect when not connected (the controller decided it is time) */
    PulseChangeCallback callback;  /* installed after a successful connect (when not empty) */
    std::string app_name = "Vinyl"; /* VINYL */
    int connect_timeout_ms = 2000;
};

struct ScanResult {
    unsigned seq = 0;
    int connect_result = 1; /* 1: no connect was attempted; 0 or the connect error */
    bool connected = false; /* connected after the job */
    bool listed = false;    /* the three lists were read */
    bool callback_set = false;
    std::vector<PulseDevice> sources, sinks;
    std::vector<PulseCard> cards;
    bool have_info = false;   /* VINYL */
    PulseServerInfo info;     /* VINYL: the default sink */
};

struct CardResult {
    unsigned seq = 0;
    std::string name;
    bool found = false;
    PulseCard card;
};

struct IdentityResult {
    unsigned seq = 0;
    bool ok = false;
    std::string actual; /* the source the server has the stream on ("" when it names none: T9, a moment of a profile switch) */
};

class PulseWorker {
public:
    explicit PulseWorker(PulseApi &api);
    ~PulseWorker(); /* stop() */
    PulseWorker(const PulseWorker &) = delete;
    PulseWorker &operator=(const PulseWorker &) = delete;

    /* false: a request of that kind is already pending or running. */
    bool request_scan(const ScanRequest &rq);
    bool request_card(const std::string &name);
    bool request_identity(std::shared_ptr<PulseCapture> stream);
    bool scan_pending() const;
    bool card_pending() const;
    bool identity_pending() const;

    /* true when a finished answer was waiting (it is moved out) */
    bool take_scan(ScanResult &out);
    bool take_card(CardResult &out);
    bool take_identity(IdentityResult &out);

    /* Ends the thread; waits for the call in progress (bounded by the server's own timeouts). Idempotent. */
    void stop();

private:
    void run();

    PulseApi &api_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    bool scan_busy_ = false, card_busy_ = false, ident_busy_ = false;
    bool scan_ready_ = false, card_ready_ = false, ident_ready_ = false;
    ScanResult scan_;
    CardResult card_;
    IdentityResult ident_;
    unsigned seq_ = 0;
    std::thread th_;
};

}  // namespace vinyl
