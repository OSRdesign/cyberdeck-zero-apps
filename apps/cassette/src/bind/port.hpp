/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding (task T6c): what the binding needs from the application controller (core/app/controller.hpp), as an
 * interface so the binding's tests drive it with a fake (tests/bind) and the app with the real Controller
 * (make_controller_port). The commands keep the Controller's own names, arguments and results; no LVGL here.
 *
 * Threads: snapshot(), takes() and peak_range() are the Controller's non-blocking reads (the UI thread polls them at
 * 30 Hz). Every other call may wait (a command takes the controller's lock; next_take_number() and edit_names() read
 * files): the binding makes them on its worker thread only (worker.hpp), never on the UI thread.
 */
#pragma once

#include "core/app/controller.hpp"

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cassette {
namespace bind {

constexpr int kRenameEmpty = -ENODATA, kRenameTooLong = -ENAMETOOLONG, kRenameBadChar = -EILSEQ,
              kRenameNotTake = -ENOTSUP;

class Port {
public:
    virtual ~Port() = default;

    /* ---- polled (any thread, non-blocking) ---- */
    virtual Snapshot snapshot() const = 0;
    virtual std::vector<TakeRow> takes() const = 0;
    virtual std::vector<PeakBar> peak_range(int64_t start_frame, uint64_t frames, int bars) const = 0;

    /* ---- commands (worker thread) ---- */
    virtual void ack_notices() = 0;
    virtual bool key_event(KeyEvent e) = 0;
    virtual bool set_level_db(double db) = 0;
    virtual bool set_auto_gain(bool on) = 0;
    virtual bool set_soft_gain_db(double db) = 0;
    virtual bool set_format(RecFormat f) = 0;
    virtual bool set_format_choice(RecFormat f, const std::string &preset) = 0;
    virtual bool set_folder(const std::string &folder) = 0;
    virtual bool set_prefix(const std::string &prefix) = 0;
    /* The Take number setting (Controller::set_next_take_number): 0, or -errno (-EINVAL range, -EBUSY recording). */
    virtual int set_next_take(uint64_t n, uint64_t &landed) = 0;
    virtual bool set_auto_split(bool on) = 0;
    virtual bool set_record_timer(int seconds) = 0;
    virtual bool set_scan_mode(ScanMode m) = 0;
    virtual bool set_low_cut(bool on) = 0;
    virtual bool set_limiter(bool on) = 0;
    virtual bool set_monitor(bool on) = 0;
    virtual bool set_monitor_level(int percent) = 0;
    virtual bool set_mute(bool on) = 0;
    virtual bool set_sync_interval(int seconds) = 0;
    virtual bool select_output(const std::string &device) = 0;
    virtual bool select_card(int index) = 0;
    virtual void refresh_cards() = 0;
    virtual void refresh_takes() = 0;
    virtual bool select_take(const std::string &name) = 0;
    virtual bool play_take(const std::string &name) = 0;
    virtual bool load(const std::string &name) = 0;         /* LOAD: the take into the deck, nothing playing */
    virtual bool preview_take(const std::string &name) = 0; /* the list's preview: plays without loading */
    /* 0 or -errno. The Controller's -EINVAL is split by cause (the old -EINVAL, an invalid prefix, stays the answer of the
     * core): kRenameEmpty, kRenameTooLong, kRenameBadChar for the new name, kRenameNotTake for a file that is no take. */
    virtual int rename_take(const std::string &name, const std::string &new_prefix) = 0;
    virtual int delete_take(const std::string &name) = 0;
    virtual bool open_trim(const std::string &take_name) = 0;
    virtual void close_trim() = 0;
    virtual bool set_position(uint64_t frame) = 0;
    virtual bool move_position(int64_t delta_frames) = 0;
    virtual bool set_in() = 0;
    virtual bool set_out() = 0;
    virtual bool add_split() = 0;
    virtual bool remove_split(int n) = 0;
    virtual bool select_split(int n) = 0;
    virtual bool set_zoom(int level) = 0;
    virtual bool preview(bool on) = 0;
    virtual bool save(SaveKind kind) = 0;
    virtual void app_leave() = 0;
    virtual void shutdown() = 0;

    /* ---- file-side helpers (worker thread) ---- */
    /* The number REC would hand out next: the take counter + 1 moved past numbers the file prefix already uses, or the
     * highest take found + 1 without a counter (peek_take_number: the rule of reserve_take_number, reserving nothing). */
    virtual uint64_t next_take_number() = 0;
    /* The names a save of `take` into `count` files would write now (next_edit_names over the folder and the take's
     * record of used numbers); the save itself reserves them again. */
    virtual std::vector<std::string> edit_names(const std::string &take, unsigned count) = 0;
};

/* The Port of a real Controller. `ops` reads the take counter and the edit records (the Controller's own FileOps). */
std::unique_ptr<Port> make_controller_port(Controller &controller, FileOps &ops);

} // namespace bind
} // namespace cassette
