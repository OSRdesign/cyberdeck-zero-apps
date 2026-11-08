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

/* The take counter file: the last number handed out, decimal (naming.hpp). 0 when missing or unreadable. */
uint64_t read_counter(FileOps &ops, const std::string &dir)
{
    const int fd = ops.open(take_counter_path(dir), O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[32] = {};
    const ssize_t n = ops.pread(fd, buf, sizeof buf - 1, 0);
    ops.close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return std::strtoull(buf, nullptr, 10);
}

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
    bool set_folder(const std::string &f) override { return c_.set_folder(f); }
    bool set_prefix(const std::string &p) override { return c_.set_prefix(p); }
    bool set_auto_split(bool on) override { return c_.set_auto_split(on); }
    bool set_record_timer(int s) override { return c_.set_record_timer(s); }
    bool set_scan_mode(ScanMode m) override { return c_.set_scan_mode(m); }
    bool set_low_cut(bool on) override { return c_.set_low_cut(on); }
    bool set_limiter(bool on) override { return c_.set_limiter(on); }
    bool select_card(int i) override { return c_.select_card(i); }
    void refresh_cards() override { c_.refresh_cards(); }
    void refresh_takes() override { c_.refresh_takes(); }
    bool select_take(const std::string &n) override { return c_.select_take(n); }
    bool play_take(const std::string &n) override { return c_.play_take(n); }
    int rename_take(const std::string &n, const std::string &p) override { return c_.rename_take(n, p); }
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
    void app_leave() override { c_.app_leave(); }
    void shutdown() override { c_.shutdown(); }

    uint64_t next_take_number() override
    {
        const std::string dir = c_.snapshot().folder;
        if (dir.empty()) return 1;
        return std::max(read_counter(ops_, dir), highest_take_found(ops_, dir)) + 1;
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
