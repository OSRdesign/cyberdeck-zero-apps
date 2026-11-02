/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the real Backend, a connection thread around the engine (no LVGL).
 *
 * One thread owns the engine, the WebSocket, the HTTP requests and the pairing listener; it sleeps in poll() on the
 * WebSocket, the listener and a wake-up eventfd until the engine's next deadline (1 s at most), so it costs nothing
 * while idle. The LVGL thread never touches the network (docs/APP-GUIDELINES.md section 6): it sends Commands (a
 * queue and the eventfd) and copies the View when its version changed (a mutex). Files are written by this thread
 * (store.hpp). The log names events and reasons, never a token, a code or a message body.
 *
 * At exit the destructor raises the stop flag: a libcurl call in progress gives up at its next progress callback,
 * and the thread is joined.
 */
#pragma once

#include "engine.hpp"
#include "net.hpp"
#include "view.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ha {

class ClientBackend : public Backend {
public:
    ClientBackend(Settings settings, Credentials creds, std::vector<std::string> favourites, std::string config_dir,
                  std::map<std::string, std::string> names = {});
    ~ClientBackend() override;
    ClientBackend(const ClientBackend &) = delete;
    ClientBackend &operator=(const ClientBackend &) = delete;

    std::uint64_t version() override { return version_.load(); }
    View snapshot() override;
    void send(const Command &c) override;

    /* the integration test: largest message and dropped ones seen on the link */
    std::size_t largest_message() const { return largest_.load(); }

private:
    void loop();
    void run_actions();
    void exec(const Action &a);
    void publish(bool force);
    void wake();
    static std::int64_t now_ms();
    static std::int64_t wall();

    Engine engine_;
    std::string dir_;
    WsLink ws_;
    Listener listener_;
    std::string fatal_; /* libcurl missing or without WebSocket */
    bool ws_kick_ = false; /* read the link at once: libcurl may already hold the first message (auth_required can
                              arrive with the upgrade's answer, and then the socket never becomes readable again) */
    std::uint64_t published_ = 0;

    std::mutex m_;
    std::deque<Command> commands_;
    View view_;
    std::atomic<std::uint64_t> version_{0};
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> largest_{0};
    int wake_fd_ = -1;
    std::thread thread_;
};

} // namespace ha
