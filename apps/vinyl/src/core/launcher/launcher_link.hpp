/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl and the launcher (APP-CONTRACT sections 5b, 5c and 14):
 *
 *   LauncherStatus   status.state (volume, mute, the paired Bluetooth audio devices) and board.state (battery, backlight,
 *                    the Bluetooth audio stack), re-read when they change, trusted only while the launcher's pid lives
 *                    (the files and the parser are Cassette's launcher_facts). The volume chip shows the system volume
 *                    from here: the launcher draws nothing over a fullscreen app.
 *   LauncherRequests launcher.sock requests on a worker thread (never the LVGL thread, at most about 200 ms, silent without
 *                    the socket): "dim allow" while music plays in front on a battery board (decision D16), "dim deny" when
 *                    it stops for a while.
 * LVGL-free.
 */
#pragma once

#include "../pulse/launcher_facts.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vinyl {

struct LauncherStatus {
    bool have_status = false, have_board = false;
    int volume = -1;          /* 0..100, -1 = no sound output or unknown */
    bool mute = false;
    int battery = -1;         /* board.state battery: 0 none, 1 present, 2 not set up, -1 unknown */
    std::string backlight;    /* "none" | "onoff" | "levels" | "" */
    int sound = -1;           /* board.state sound: 0 / 1 / 2, -1 unknown */
    std::string requests;     /* the verbs launcher.sock answers */
    LauncherFacts facts;      /* bt_audio_dev, bt_audio, bt_hfp */
    bool battery_board() const { return battery == 1 && backlight == "levels"; }
    bool operator==(const LauncherStatus &o) const;
    bool operator!=(const LauncherStatus &o) const { return !(*this == o); }
};

/* The two texts (and whether each file's pid is alive) into the status. */
void parse_launcher_status(const std::string &status_text, bool status_trusted, const std::string &board_text,
                           bool board_trusted, LauncherStatus &out);

class LauncherStatusReader {
public:
    explicit LauncherStatusReader(std::unique_ptr<FactsIo> io = nullptr, std::string status_path = "",
                                  std::string board_path = "");
    /* Looks at the files at most once a second; true when the status changed. */
    bool poll(uint64_t now_ms);
    const LauncherStatus &status() const { return st_; }

private:
    std::unique_ptr<FactsIo> io_;
    std::string status_path_, board_path_, status_stamp_, board_stamp_;
    std::string status_text_, board_text_;
    LauncherStatus st_;
    uint64_t last_ = 0;
    bool looked_ = false;
};

/* The socket path: $XDG_RUNTIME_DIR/applaunch/launcher.sock ("" without XDG_RUNTIME_DIR). */
std::string launcher_socket_path();
/* One request line, one reply line, within timeout_ms: 0 ok, 1 err, -1 no service. */
int launcher_call(const std::string &socket_path, const std::string &request, std::string *reply, int timeout_ms = 200);

class LauncherRequests {
public:
    explicit LauncherRequests(std::string socket_path = launcher_socket_path());
    ~LauncherRequests();
    /* Queued; a newer request of the same verb replaces one still waiting. */
    void send(const std::string &request);
    int sent() const { return sent_.load(); }

private:
    void run();
    std::string path_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    bool quit_ = false;
    std::atomic<int> sent_{0};
    uint64_t dead_until_ = 0;
    std::thread th_;
};

}  // namespace vinyl
