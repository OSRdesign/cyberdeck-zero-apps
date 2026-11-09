/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding tests: a fake Port. Every command is logged as one line ("key_event Rec", "set_soft_gain_db 3.0") and
 * changes the fake's snapshot the simple way the controller would, so the tests can check both the call and what the
 * view model then shows. Thread-safe (the threaded Worker calls it). Synthetic values only.
 */
#pragma once

#include "bind/port.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace tb {

using namespace cassette;

inline const char *key_name(KeyEvent e)
{
    switch (e) {
    case KeyEvent::Rec: return "Rec";
    case KeyEvent::Stop: return "Stop";
    case KeyEvent::PlayPause: return "PlayPause";
    case KeyEvent::RewDown: return "RewDown";
    case KeyEvent::RewUp: return "RewUp";
    case KeyEvent::FwdDown: return "FwdDown";
    case KeyEvent::FwdUp: return "FwdUp";
    case KeyEvent::Esc: return "Esc";
    case KeyEvent::TapOverlay: return "TapOverlay";
    case KeyEvent::AppLeave: return "AppLeave";
    }
    return "?";
}

inline std::string num(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

class FakePort final : public bind::Port {
public:
    mutable std::mutex mu;
    Snapshot snap;
    std::vector<TakeRow> rows;
    std::vector<std::string> calls;
    double level_step = 1.0;      /* set_level_db rounds to this step (a coarse card: 3.0) */
    bool open_trim_result = true;
    int delete_result = 0, rename_result = 0;
    bool save_result = true, folder_result = true, output_result = true, load_result = true;
    uint64_t next = 8;
    std::vector<std::string> names = {"take_006_20261108-1105-1.wav", "take_006_20261108-1105-2.wav",
                                      "take_006_20261108-1105-3.wav"};
    std::vector<PeakBar> peaks_answer; /* empty: bars of 16384 */
    int peak_calls = 0;

    void log(const std::string &s)
    {
        std::lock_guard<std::mutex> g(mu);
        calls.push_back(s);
    }
    std::vector<std::string> take_calls()
    {
        std::lock_guard<std::mutex> g(mu);
        std::vector<std::string> c;
        c.swap(calls);
        return c;
    }
    bool called(const std::string &s)
    {
        std::lock_guard<std::mutex> g(mu);
        for (const std::string &c : calls)
            if (c == s) return true;
        return false;
    }
    void set(const Snapshot &s)
    {
        std::lock_guard<std::mutex> g(mu);
        snap = s;
    }

    Snapshot snapshot() const override
    {
        std::lock_guard<std::mutex> g(mu);
        return snap;
    }
    std::vector<TakeRow> takes() const override
    {
        std::lock_guard<std::mutex> g(mu);
        return rows;
    }
    std::vector<PeakBar> peak_range(int64_t start, uint64_t frames, int bars) const override
    {
        std::lock_guard<std::mutex> g(mu);
        const_cast<FakePort *>(this)->peak_calls++;
        const_cast<FakePort *>(this)->calls.push_back("peak_range " + std::to_string(start) + " " + std::to_string(frames) +
                                                      " " + std::to_string(bars));
        if (!peaks_answer.empty()) return peaks_answer;
        PeakBar b;
        b.min = -16384;
        b.max = 16384;
        return std::vector<PeakBar>(static_cast<size_t>(bars), b);
    }

    void ack_notices() override
    {
        log("ack_notices");
        std::lock_guard<std::mutex> g(mu);
        snap.notices.clear();
    }
    bool key_event(KeyEvent e) override
    {
        log(std::string("key_event ") + key_name(e));
        std::lock_guard<std::mutex> g(mu);
        if (e == KeyEvent::Stop && snap.deck.state >= kDeckPlaying) { /* a playback or preview stops (and rewinds) */
            snap.deck.state = kDeckIdle;
            snap.take.playing_name.clear();
            snap.time.position_s = 0;
        }
        return true;
    }
    bool set_level_db(double db) override
    {
        log("set_level_db " + num(db));
        std::lock_guard<std::mutex> g(mu);
        const double base = snap.card.level_min_db;
        const double v = base + level_step * static_cast<long>((db - base) / level_step + 0.5);
        snap.card.level_db = std::max(snap.card.level_min_db, std::min(snap.card.level_max_db, v));
        return true;
    }
    bool set_auto_gain(bool on) override
    {
        log(std::string("set_auto_gain ") + (on ? "on" : "off"));
        return true;
    }
    bool set_soft_gain_db(double db) override
    {
        log("set_soft_gain_db " + num(db));
        std::lock_guard<std::mutex> g(mu);
        snap.card.soft_gain_db = db;
        return true;
    }
    bool set_format(RecFormat f) override
    {
        log(std::string("set_format ") + format_key(f));
        return true;
    }
    bool set_folder(const std::string &f) override
    {
        log("set_folder " + f);
        return folder_result;
    }
    bool set_prefix(const std::string &p) override
    {
        log("set_prefix " + p);
        return valid_prefix(p);
    }
    bool set_auto_split(bool on) override
    {
        log(std::string("set_auto_split ") + (on ? "on" : "off"));
        return true;
    }
    bool set_record_timer(int s) override
    {
        log("set_record_timer " + std::to_string(s));
        return true;
    }
    bool set_scan_mode(ScanMode m) override
    {
        log(std::string("set_scan_mode ") + scan_mode_key(m));
        return true;
    }
    bool set_low_cut(bool on) override
    {
        log(std::string("set_low_cut ") + (on ? "on" : "off"));
        return true;
    }
    bool set_limiter(bool on) override
    {
        log(std::string("set_limiter ") + (on ? "on" : "off"));
        return true;
    }
    bool set_mute(bool on) override
    {
        log(std::string("set_mute ") + (on ? "on" : "off"));
        return true;
    }
    bool set_sync_interval(int s) override
    {
        log("set_sync_interval " + std::to_string(s));
        return true;
    }
    bool select_output(const std::string &d) override
    {
        log("select_output " + d);
        return output_result;
    }
    bool select_card(int i) override
    {
        log("select_card " + std::to_string(i));
        return true;
    }
    void refresh_cards() override { log("refresh_cards"); }
    void refresh_takes() override { log("refresh_takes"); }
    bool select_take(const std::string &n) override
    {
        log("select_take " + n);
        return true;
    }
    bool play_take(const std::string &n) override
    {
        log("play_take " + n);
        return true;
    }
    bool load(const std::string &n) override
    {
        log("load " + n);
        std::lock_guard<std::mutex> g(mu);
        if (load_result) {
            snap.take.name = n;
            for (TakeRow &t : rows) t.current = t.name == n;
            ++snap.takes_revision;
            ++snap.take.load_seq;
            snap.deck.state = kDeckIdle;
            snap.take.playing_name.clear();
        }
        return load_result;
    }
    bool preview_take(const std::string &n) override
    {
        log("preview_take " + n);
        std::lock_guard<std::mutex> g(mu);
        snap.deck.state = kDeckPlaying; /* the preview plays the take; the loaded take stays */
        snap.take.playing_name = n;
        return true;
    }
    int rename_take(const std::string &n, const std::string &p) override
    {
        log("rename_take " + n + " " + p);
        return rename_result;
    }
    int delete_take(const std::string &n) override
    {
        log("delete_take " + n);
        return delete_result;
    }
    bool open_trim(const std::string &n) override
    {
        log("open_trim " + n);
        return open_trim_result;
    }
    void close_trim() override
    {
        log("close_trim");
        std::lock_guard<std::mutex> g(mu);
        snap.trim = TrimSnap();
    }
    bool set_position(uint64_t f) override
    {
        log("set_position " + std::to_string(f));
        return true;
    }
    bool move_position(int64_t d) override
    {
        log("move_position " + std::to_string(d));
        return true;
    }
    bool set_in() override
    {
        log("set_in");
        return true;
    }
    bool set_out() override
    {
        log("set_out");
        return true;
    }
    bool add_split() override
    {
        log("add_split");
        return true;
    }
    bool remove_split(int n) override
    {
        log("remove_split " + std::to_string(n));
        return true;
    }
    bool select_split(int n) override
    {
        log("select_split " + std::to_string(n));
        return true;
    }
    bool set_zoom(int z) override
    {
        log("set_zoom " + std::to_string(z));
        return true;
    }
    bool preview(bool on) override
    {
        log(std::string("preview ") + (on ? "on" : "off"));
        std::lock_guard<std::mutex> g(mu);
        snap.trim.previewing = on && snap.trim.open;
        return true;
    }
    bool save(SaveKind k) override
    {
        log(std::string("save ") + (k == SaveKind::NewFiles ? "new" : "replace"));
        return save_result;
    }
    void app_leave() override { log("app_leave"); }
    void shutdown() override { log("shutdown"); }
    uint64_t next_take_number() override
    {
        log("next_take_number");
        return next;
    }
    std::vector<std::string> edit_names(const std::string &take, unsigned count) override
    {
        log("edit_names " + take + " " + std::to_string(count));
        std::vector<std::string> out(names.begin(), names.begin() + std::min<size_t>(count, names.size()));
        return out;
    }
};

/* A snapshot with a card (level control and auto gain), idle, 48k/16 mono WAV, the default settings. */
inline Snapshot base_snapshot()
{
    Snapshot s;
    s.started = true;
    s.deck.state = kDeckIdle;
    s.deck.rew.enabled = s.deck.play_pause.enabled = s.deck.fwd.enabled = true;
    s.deck.rec.enabled = true;
    s.card.present = true;
    s.card.label = "Demo Microphone";
    s.card.labels = {"Demo Microphone", "Demo Line Input"};
    s.card.selected = 0;
    s.card.has_level = s.card.has_agc = true;
    s.card.level_db = 11.9;
    s.card.level_min_db = 0;
    s.card.level_max_db = 23.81;
    s.card.soft_gain_db = 6.0;
    s.take.format_text = "48k/16 WAV";
    s.take.free_bytes = 41ull << 30;
    s.take.hours_left = 12.0 + 40.0 / 60.0;
    s.meter.channels = 1;
    s.meter.ch[0].bar = 11;
    s.meter.ch[0].hold_bar = 12;
    s.folder = "/home/user/Recordings";
    return s;
}

inline TakeRow row(const std::string &name, double seconds, uint64_t size, const std::string &date, bool current = false)
{
    TakeRow r;
    r.name = name;
    TakeName tn;
    r.label = parse_take_name(name, tn) ? take_label(tn) : file_stem(name);
    r.path = "/home/user/Recordings/" + name;
    r.seconds = seconds;
    r.size = size;
    r.rate = 48000;
    r.channels = 1;
    r.bits = 16;
    r.format_text = "48k/16 WAV";
    r.date_text = date;
    r.current = current;
    return r;
}

} // namespace tb
