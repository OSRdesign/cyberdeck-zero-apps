/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette binding: the Port of a real Controller (see port.hpp).
 */
#include "port.hpp"

#include "core/takes/naming.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>

namespace cassette {
namespace bind {

namespace {

class ControllerPort final : public Port {
public:
    ControllerPort(Controller &c, FileOps &ops) : c_(c), ops_(ops) {}

    Snapshot snapshot() const override { return c_.snapshot(); }
    std::vector<TakeRow> takes() const override { return c_.takes(); }
    std::vector<PeakBar> peak_range(int64_t start, uint64_t frames, int bars) const override
    {
        return c_.peak_range(start, frames, bars);
    }

    void ack_notices() override { c_.ack_notices(); }
    bool key_event(KeyEvent e) override { return c_.key_event(e); }
    bool set_level_db(double db) override { return c_.set_level_db(db); }
    bool set_auto_gain(bool on) override { return c_.set_auto_gain(on); }
    bool set_soft_gain_db(double db) override { return c_.set_soft_gain_db(db); }
    bool set_format(RecFormat f) override { return c_.set_format(f); }
    bool set_format_choice(RecFormat f, const std::string &p) override { return c_.set_format_choice(f, p); }
    bool set_rec_params(RecFormat f, const std::string &p, int rate, int quality) override
    {
        return c_.set_rec_params(f, p, rate, quality);
    }
    bool set_folder(const std::string &f) override { return c_.set_folder(f); }
    bool set_prefix(const std::string &p) override { return c_.set_prefix(p); }
    int set_next_take(uint64_t n, uint64_t &landed) override { return c_.set_next_take_number(n, &landed); }
    bool set_auto_split(bool on) override { return c_.set_auto_split(on); }
    bool set_record_timer(int s) override { return c_.set_record_timer(s); }
    bool set_scan_mode(ScanMode m) override { return c_.set_scan_mode(m); }
    bool set_low_cut(bool on) override { return c_.set_low_cut(on); }
    bool set_limiter(bool on) override { return c_.set_limiter(on); }
    bool set_monitor(bool on) override { return c_.set_monitor(on); }
    bool set_wave_fit(bool on) override { return c_.set_wave_fit(on); }
    bool set_monitor_level(int p) override { return c_.set_monitor_level(p); }
    bool set_mute(bool on) override { return c_.set_mute(on); }
    bool set_sync_interval(int s) override { return c_.set_sync_interval(s); }
    bool select_output(const std::string &d) override { return c_.select_output(d); }
    bool select_card(int i) override { return c_.select_card(i); }
    void refresh_cards() override { c_.refresh_cards(); }
    void set_page(UiPage p) override { c_.set_page(p); }
    void refresh_takes() override { c_.refresh_takes(); }
    bool select_take(const std::string &n) override { return c_.select_take(n); }
    bool play_take(const std::string &n) override { return c_.play_take(n); }
    bool load(const std::string &n) override { return c_.load(n); }
    bool preview_take(const std::string &n) override { return c_.preview_take(n); }
    int rename_take(const std::string &n, const std::string &p) override
    {
        RenameRefusal why = RenameRefusal::None;
        const int r = c_.rename_take(n, p, nullptr, &why);
        if (r != -EINVAL) return r;
        switch (why) {
        case RenameRefusal::Empty: return kRenameEmpty;
        case RenameRefusal::TooLong: return kRenameTooLong;
        case RenameRefusal::BadChar: return kRenameBadChar;
        case RenameRefusal::NotTake: return kRenameNotTake;
        default: return r;
        }
    }
    int delete_take(const std::string &n) override { return c_.delete_take(n); }
    bool open_trim(const std::string &n) override { return c_.open_trim(n); }
    void close_trim() override { c_.close_trim(); }
    bool set_position(uint64_t f) override { return c_.set_position(f); }
    bool move_position(int64_t d) override { return c_.move_position(d); }
    bool set_in() override { return c_.set_in(); }
    bool set_out() override { return c_.set_out(); }
    bool add_split() override { return c_.add_split(); }
    bool remove_split(int n) override { return c_.remove_split(n); }
    bool select_split(int n) override { return c_.select_split(n); }
    bool set_zoom(int z) override { return c_.set_zoom(z); }
    bool preview(bool on) override { return c_.preview(on); }
    bool save(SaveKind k) override { return c_.save(k); }
    MergeInfo plan_merge(const std::vector<std::string> &n) override { return c_.plan_merge(n); }
    bool start_merge(const std::vector<std::string> &n, bool del) override { return c_.start_merge(n, del); }
    void cancel_merge() override { c_.cancel_merge(); }
    void app_leave() override { c_.app_leave(); }
    void shutdown() override { c_.shutdown(); }

    uint64_t next_take_number() override
    {
        const Snapshot s = c_.snapshot();
        if (s.folder.empty()) return 1;
        return peek_take_number(ops_, s.folder, s.settings.prefix);
    }

    std::vector<std::string> edit_names(const std::string &take, unsigned count) override
    {
        const std::string dir = c_.snapshot().folder;
        std::vector<std::string> names;
        if (dir.empty() || ops_.list_dir(dir, names) < 0) names.clear();
        return next_edit_names(take, count, names, read_edit_record(ops_, dir, take));
    }

private:
    Controller &c_;
    FileOps &ops_;
};

} // namespace

std::unique_ptr<Port> make_controller_port(Controller &controller, FileOps &ops)
{
    return std::unique_ptr<Port>(new ControllerPort(controller, ops));
}

} // namespace bind
} // namespace cassette
