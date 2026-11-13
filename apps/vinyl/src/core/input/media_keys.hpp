/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl input: the transport keys of headsets, speakers and keyboards (report 059, decision D9: the player reads the
 * media-key devices itself; the launcher passes only volume, mute and brightness, contract section 14).
 *
 * A reader thread lists /proc/bus/input/devices about every 2 s (a Bluetooth headset's "(AVRCP)" node comes and goes),
 * opens every event node that advertises one of the transport keys read-only and never grabbed, and turns presses into
 * actions:
 *
 *   KEY_PLAYPAUSE (164), KEY_PLAYCD (200), KEY_PAUSECD (201)   play / pause (a toggle: the HackBerry's speaker node sends
 *                                                              PLAYCD and PAUSECD from one button, not PLAYPAUSE)
 *   KEY_NEXTSONG (163)                                         next track
 *   KEY_PREVIOUSSONG (165)                                     previous track
 *   KEY_STOPCD (166)                                           stop (pause, back to the start of the track)
 *
 * Presses count (value 1, not the auto-repeat 2); for next and previous the release (value 0) counts too, from the node
 * whose press acted, so a held button is a long press (the transport press, core/transport: a scan; owner ruling
 * 2026-11-12). A node that goes away while its button is held gives the release. A press seen twice within 250 ms (the
 * same button on two nodes, or here and through the toolkit's keyboard path while the app is in front) acts once
 * (Dedupe), and so does its release (MediaPresses). While
 * the app is hidden the launcher grabs the keyboards, so a keyboard's own media keys reach Vinyl only in front; headset
 * and speaker nodes are no keyboards and keep working while hidden. LVGL-free.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {

enum class MediaAction { None, Toggle, Next, Prev, Stop };
const char *media_action_name(MediaAction a);
MediaAction media_action_for(unsigned key_code);

struct InputDevice {
    std::string name;
    std::string event;           /* "event10" */
    std::vector<uint64_t> keys;  /* the KEY= bitmap, word 0 = bits 0..63 */
    bool has_key(unsigned code) const;
    bool media() const;          /* advertises a transport key */
};

/* The devices of a /proc/bus/input/devices text. */
std::vector<InputDevice> parse_input_devices(const std::string &text);

/* Acts once per press across the paths (see above). */
class Dedupe {
public:
    explicit Dedupe(uint64_t window_ms = 250) : window_(window_ms) {}
    bool accept(MediaAction a, uint64_t now_ms);

private:
    uint64_t window_;
    std::map<int, uint64_t> last_;
};

/* A press (down) or, for next and previous, a release (up). */
struct MediaEvent {
    MediaAction action = MediaAction::None;
    bool down = true;
    bool operator==(const MediaEvent &o) const { return action == o.action && down == o.down; }
};

/* The key events of every source ("event10", "lvgl") into media events: presses through the Dedupe, the release of next
 * and previous only from the source whose press acted. */
class MediaPresses {
public:
    explicit MediaPresses(uint64_t window_ms = 250) : dedupe_(window_ms) {}
    /* value: 1 press, 0 release, 2 auto-repeat. True when it acts (`ev` filled). */
    bool feed(const std::string &source, MediaAction a, int value, uint64_t now_ms, MediaEvent &ev);
    /* The source went away: the releases of the presses it holds. */
    std::vector<MediaEvent> drop_source(const std::string &source);

private:
    Dedupe dedupe_;
    std::map<int, std::string> held_; /* next / previous -> the source whose press acted */
};

class MediaKeyReader {
public:
    MediaKeyReader() = default;
    ~MediaKeyReader();
    /* `devices_path` / `dev_dir`: the tests point them at fixtures. */
    void start(const std::string &devices_path = "/proc/bus/input/devices", const std::string &dev_dir = "/dev/input");
    void stop();
    /* The events since the last call, oldest first (already deduplicated among the nodes). */
    std::vector<MediaEvent> take();
    /* The nodes open now, for the log ("XWF-M18-M28-M38 (AVRCP) event10"). */
    std::vector<std::string> open_nodes() const;
    /* A key that came another way (the keyboard path in front; value as MediaPresses::feed): true when it acts. */
    bool accept_other(MediaAction a, int value, MediaEvent &ev);

private:
    void run();
    void rescan();

    std::string devices_path_, dev_dir_;
    std::thread th_;
    std::atomic<bool> quit_{false};
    mutable std::mutex mu_;
    std::deque<MediaEvent> pending_;
    std::map<std::string, int> fds_;   /* event name -> fd */
    std::map<std::string, std::string> names_;
    MediaPresses presses_;
    void close_node(const std::string &event); /* with mu_ held */
};

uint64_t mono_ms_now();

}  // namespace vinyl
