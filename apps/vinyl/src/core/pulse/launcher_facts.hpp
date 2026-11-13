/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette: the launcher's read-only Bluetooth audio facts (task T8 of the Bluetooth release). The launcher (0.10.1 and later)
 * writes two small text files under $APPLAUNCH_STATE_DIR, else $XDG_RUNTIME_DIR/applaunch/, else /tmp/applaunch-<name>:
 *
 *   status.state  key=value lines, v=1; `bt_audio_dev=MAC:kind:connected[:a2dp|hfp][:...];...` (MAC with underscores,
 *                 kind headset|headphones|speaker, connected 0|1, at most 8 entries, always the last key, absent when none)
 *   board.state   v=1; `bt_audio=pipewire|pulseaudio|bluealsa|none`, `bt_hfp=0|1`, `bt_le_audio=0|1`
 *
 * Both are rewritten by atomic rename and trusted only while their `pid` is alive (kill(pid, 0)). This module has no SDK
 * dependency: the parser is pure, LauncherFactsProvider reads and re-reads the files through an injectable `FactsIo` (a
 * stat of both files about once a second on the controller's slow tick; never blocks beyond one small read), and the
 * merge helpers turn the facts into the controller's lists: kind and profile hints for the connected devices, dimmed rows for
 * the paired ones that are not connected. Without the files (an older launcher, no launcher: standalone runs, scenes, tests)
 * every fact is absent and the controller behaves exactly as before. LVGL-free.
 */
#pragma once

#include "bt_input.hpp"
#include "bt_output.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vinyl {

/* One paired Bluetooth audio device of status.state's bt_audio_dev. */
struct LauncherBtDev {
    std::string mac;      /* "70:BF:92:53:6B:8B" (upper case, colons) */
    std::string kind;     /* "headset", "headphones", "speaker" (an unknown word is kept as it is: treat it as "other") */
    bool connected = false;
    std::string profile;  /* "" | "a2dp" | "hfp" (an unknown word gives "") */
};

struct LauncherFacts {
    bool have_status = false;               /* status.state read and trusted */
    bool have_board = false;                /* board.state read and trusted */
    std::vector<LauncherBtDev> devices;     /* paired audio devices, in the launcher's order, at most 8 */
    std::string bt_audio;                   /* "pipewire" | "pulseaudio" | "bluealsa" | "none" | "" (absent or unknown word) */
    int bt_hfp = -1;                        /* 0 | 1 | -1 absent */
    int bt_le_audio = -1;                   /* 0 | 1 | -1 absent */

    const LauncherBtDev *find(const std::string &mac) const; /* any case, colons or underscores */
    /* The launcher says no sound stack carries Bluetooth audio on this board. */
    bool stack_none() const { return bt_audio == "none"; }
    /* The launcher says a sound server (PipeWire or PulseAudio) owns the Bluetooth endpoints. */
    bool stack_server() const { return bt_audio == "pipewire" || bt_audio == "pulseaudio"; }
    bool operator==(const LauncherFacts &o) const;
    bool operator!=(const LauncherFacts &o) const { return !(*this == o); }
};

constexpr size_t kLauncherBtDevMax = 8;

/* "70_BF_92_53_6B_8B" or "70:BF:92:53:6B:8B" in any case -> "70:BF:92:53:6B:8B"; "" when the text is not an address. */
std::string launcher_mac(const std::string &text);

/* The bt_audio_dev value. Tolerant: an entry that does not parse (no MAC, no kind, connected not 0 or 1) is skipped, fields
 * after the profile and unknown words are ignored, more than 8 entries keep the first 8. */
std::vector<LauncherBtDev> parse_bt_audio_dev(const std::string &value);

/* A state file's text: its lines, `v` (must be 1) and `pid`. The values of the keys are in `kv` (the last of a repeated key). */
struct StateText {
    bool ok = false;           /* v=1 present */
    long pid = -1;
    std::map<std::string, std::string> kv;
};
StateText parse_state_text(const std::string &text);

/* status.state / board.state into the facts (have_status / have_board stay what the caller says: they are the pid check's). */
void apply_status_text(const StateText &st, LauncherFacts &out);
void apply_board_text(const StateText &st, LauncherFacts &out);

/* The state file path: the same order as the SDK's cp0_app_state_path(): `state_dir` (APPLAUNCH_STATE_DIR) when not empty, else
 * "<runtime_dir>/applaunch/<file>" (XDG_RUNTIME_DIR), else "/tmp/applaunch-<file>". */
std::string launcher_state_path(const std::string &file, const std::string &state_dir, const std::string &runtime_dir);

/* The file system as the provider sees it (tests give a fake). */
class FactsIo {
public:
    virtual ~FactsIo() = default;
    /* A stamp that changes when the file is rewritten (mtime, size, inode); false when the file is missing. */
    virtual bool stat(const std::string &path, std::string &stamp) = 0;
    /* The whole file (at most `max` bytes); false when missing. */
    virtual bool read(const std::string &path, std::string &text, size_t max) = 0;
    virtual bool pid_alive(long pid) = 0;
};
std::unique_ptr<FactsIo> make_posix_facts_io();

/* Reads the two files when they change. Not thread-safe: the controller calls it under its lock. */
class LauncherFactsProvider {
public:
    static constexpr uint64_t kPollMs = 1000; /* how often the files are looked at */
    /* `status_path` / `board_path` empty: the real paths from the environment. */
    explicit LauncherFactsProvider(std::unique_ptr<FactsIo> io = nullptr, std::string status_path = "", std::string board_path = "");

    /* Looks at the files when kPollMs passed since the last look (the first call always looks). true when the facts changed. */
    bool poll(uint64_t now_ms);
    const LauncherFacts &facts() const { return facts_; }
    const std::string &status_path() const { return status_path_; }
    const std::string &board_path() const { return board_path_; }

private:
    struct Watched {
        std::string path, stamp, text;
        bool present = false;
    };
    void look(Watched &w, bool &changed);

    std::unique_ptr<FactsIo> io_;
    std::string status_path_, board_path_;
    Watched status_, board_;
    LauncherFacts facts_;
    uint64_t last_ms_ = 0;
    bool looked_ = false;
};

/* ---- merging the facts into the controller's lists ---- */

/* "Not connected: connect it in Settings > Bluetooth" (a paired device that is not connected). */
extern const char *const kBtPairedReason;

/* The facts of a connected device: the launcher's kind wins (kind of the entry), the launcher's profile is the hint until the
 * sound server's card says one (hint_profile "unknown" only). A connected device whose launcher kind is not a headset leaves the
 * input list (a speaker or headphones have no microphone). Without a fact for a device nothing changes. */
void bt_facts_apply_inputs(std::vector<BtInput> &list, const LauncherFacts &facts);
void bt_facts_apply_outputs(std::vector<BtOutput> &list, const LauncherFacts &facts);

/* Paired audio devices that are not connected and not in `listed` (the server's connected ones), as dimmed rows (connected false,
 * usable false, paired true, reason kBtPairedReason). Inputs: kind headset only; outputs: headset, headphones, speaker. `names` maps
 * an upper-case MAC to the last label seen; without one: "Bluetooth device <last 5 characters of the MAC>". */
std::vector<BtInput> bt_paired_inputs(const std::vector<BtInput> &listed, const LauncherFacts &facts,
                                      const std::map<std::string, std::string> &names);
std::vector<BtOutput> bt_paired_outputs(const std::vector<BtOutput> &listed, const LauncherFacts &facts,
                                        const std::map<std::string, std::string> &names);

/* "Bluetooth device 6B:8B". */
std::string bt_generic_name(const std::string &mac);

}  // namespace vinyl
