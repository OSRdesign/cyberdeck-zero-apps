/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette takes: the settings file, ~/.config/cassette/settings.json (path injectable). Versioned ("version": 1),
 * written atomically (temp, fsync, rename, fsync dir), read tolerantly: a missing or corrupt file, an unknown or
 * invalid value gives the default and never throws; out-of-range numbers are clamped; keys this version does not know
 * (at the top level and inside a card entry) are kept on the next write.
 *
 * Per-card state: the mixer resets at every reboot, so the app stores the card's level, auto gain and mute per card and
 * re-applies them at start. A card is keyed by card_descriptor(name, usb by-id, vid:pid): the index changes across
 * reboots and some headsets share a name (report 058), so the by-id path is part of the key; a vid:pid of 0000:0000
 * carries no identity and is left out. LVGL-free.
 *
 * File format (all keys optional):
 *   { "version": 1, "format": "wav"|"flac"|"opus", "format_preset": "speech24"|..., "rate": Hz, "quality": 16|24|kbps,
 *     "folder": "~/Recordings", "prefix": "take",
 *     "sync_interval_s": 5..30, "auto_split": true (4 GiB parts) | false (one RF64 file),
 *     "record_timer_s": 0|3|10|20, "rew_fwd": "scan" (5x, held) | "jump" (5 s per tap),
 *     "low_cut": bool, "limiter": bool, "soft_gain_db": 0..24,
 *     "monitor": bool (default false), "monitor_level": 0..100 in steps of 5 (default 50),
 *     "wave_fit": true (the trim waveform is scaled to fit) | absent (true level; written only when true),
 *     "card": "<descriptor>", "output_device": "<alsa device>" | "bt:AA:BB:CC:DD:EE:FF",
     "monitor_output": "" (automatic) | "bt:AA:BB:CC:DD:EE:FF" | "<alsa device>" (absent: automatic),
 *     "bt_names": { "AA:BB:CC:DD:EE:FF": "<last label seen>" } (T8: the names of Bluetooth devices, for a paired one that is not
 *     connected; at most 16, absent when empty),
 *     "cards": { "<descriptor>": { "level_db": number (absent: no level control), "auto_gain": bool, "mute": bool } } }
 */
#pragma once

#include "../fileops.hpp"
#include "json.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cassette {

enum class RecFormat { Wav, Flac, Opus }; /* FLAC and Opus record when the card and the libraries allow (codec/formats.hpp) */
enum class ScanMode { Fast, Jump };       /* REW/FWD: fast scan at 5x while held, or a 5 s jump per tap */

constexpr int kSyncMin = 5, kSyncMax = 30, kSyncDefault = 10;
constexpr double kSoftGainMin = 0.0, kSoftGainMax = 24.0;
constexpr size_t kBtNamesMax = 16;       /* "bt_names": the most devices whose last label is remembered */
constexpr double kCardLevelMin = -120.0, kCardLevelMax = 60.0; /* a sanity range only; the mixer clamps further */
extern const int kRecordTimerChoices[4];                       /* 0 (Off), 3, 10, 20 seconds */

struct Settings {
    RecFormat format = RecFormat::Wav;
    std::string format_preset; /* "format_preset": a preset of the format (codec/formats.hpp), "" = its default */
    int rate = 0;    /* "rate": the capture sample rate in Hz the user chose; 0 = the card's default for the codec */
    int quality = 0; /* "quality": WAV / FLAC: the bit depth (16 or 24); Opus: the bit rate in kbps; 0 = the default (a
                      * preset's bit rate, else 16-bit, else Opus 32 kbps) */
    std::string folder = "~/Recordings";
    std::string prefix = "take";
    int sync_interval_s = kSyncDefault;
    bool auto_split = true;
    int record_timer_s = 0;
    ScanMode rew_fwd = ScanMode::Fast;
    bool low_cut = false;
    bool limiter = false;
    double soft_gain_db = 0.0;
    bool monitor = false;      /* hear the microphone in the card's playback side while the recorder runs (never during playback) */
    int monitor_level = 50;    /* percent 0..100, steps of 5 */
    bool wave_fit = false;     /* the trim page's waveform scale: false TRUE (real level), true FIT (the take's loudest peak fills the panel); every take opens in it */
    std::string card;          /* the chosen capture card as card_descriptor() text; empty: the first capture card */
    std::string output_device; /* the playback device the user picked ("hw:CARD=..,DEV=0", "default" or "bt:<MAC>"); empty: automatic */
    std::string monitor_output; /* where the live monitor plays: "" automatic (the capture card's playback side, or the Bluetooth headset
                                 * that is the input), "bt:<MAC>" or an ALSA device as output_device spells it */
    std::map<std::string, std::string> bt_names; /* "bt_names": upper-case MAC with colons -> the last label seen (T8; empty: absent) */
};

struct CardState {
    bool has_level = false; /* false: the card has no level control (or none was stored) */
    double level_db = 0.0;
    bool auto_gain = false;
    bool mute = false;
};

/* "name|by-id|vid:pid"; vid:pid "0000:0000" (or empty) gives "name|by-id|". */
std::string card_descriptor(const std::string &name, const std::string &usb_by_id, const std::string &vid_pid);

std::string default_settings_path(const std::string &home); /* <home>/.config/cassette/settings.json */

/* Brings every value into its range (record timer: the nearest choice; prefix/folder: the default when invalid). */
void clamp_settings(Settings &s);

const char *format_key(RecFormat f);
const char *scan_mode_key(ScanMode m);

class SettingsStore {
public:
    SettingsStore(FileOps &ops, std::string path) : ops_(ops), path_(std::move(path)) {}

    /* Reads the file. 0 when read, -ENOENT when missing, -EINVAL when not a JSON object; the values are the defaults
     * (merged with whatever was valid) in every case. */
    int load();
    /* Writes the file atomically (creates the folder). 0 or -errno; on an error the old file is untouched. */
    int save();

    Settings &values() { return s_; }
    const Settings &values() const { return s_; }
    const std::string &path() const { return path_; }

    /* The stored state of a card; false when there is none. */
    bool card_state(const std::string &descriptor, CardState &out) const;
    void set_card_state(const std::string &descriptor, const CardState &st);
    bool forget_card(const std::string &descriptor);
    std::vector<std::string> cards() const;

private:
    FileOps &ops_;
    std::string path_;
    Settings s_;
    std::map<std::string, CardState> cards_;
    std::vector<std::string> forgotten_;
    json::Value raw_ = json::Value::object(); /* the file as read, for its unknown keys */
};

}  // namespace cassette
