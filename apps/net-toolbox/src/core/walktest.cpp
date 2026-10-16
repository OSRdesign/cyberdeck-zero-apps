/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox 0.4: the Wi-Fi walk test (see walktest.hpp).
 */
#include "walktest.hpp"

#include "interfaces.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>

namespace nt {

bool proc_wireless_dbm(const std::string &text, const std::string &iface, int &dbm)
{
    for (const WirelessLevel &w : parse_net_wireless(text)) {
        if (w.iface != iface) continue;
        if (!(w.level_dbm > -200 && w.level_dbm < 0)) return false;
        dbm = static_cast<int>(std::lround(w.level_dbm));
        return true;
    }
    return false;
}

WalkReading read_walk_signal(const Source &src, CommandRunner &cmd, const std::string &iface, bool ask_iw,
                             const CancelToken &cancel, bool *iw_missing)
{
    WalkReading r;
    int dbm = 0;
    if (proc_wireless_dbm(src.read("/proc/net/wireless"), iface, dbm)) {
        r.linked = true;
        r.dbm = dbm;
    }
    if (!ask_iw || (iw_missing && *iw_missing)) return r;
    CommandSpec spec;
    spec.argv = iw_link_argv(iface);
    spec.timeout_ms = kIwTimeoutMs;
    spec.max_output = 16 * 1024;
    const CommandResult res = cmd.run(spec, cancel);
    if (!res.started || res.exit_code == 127) {
        if (iw_missing) *iw_missing = true;
        return r;
    }
    if (!res.ok()) return r; /* "command failed: No such device (-19)": the adapter went away */
    const IwLink link = parse_iw_link(res.output);
    r.has_link = true;
    if (link.connected) {
        r.bssid = link.bssid;
        r.ssid = clean_ssid(link.ssid);
        if (!r.linked && link.has_signal && link.signal_dbm < 0) {
            r.linked = true;
            r.dbm = link.signal_dbm;
            r.from_iw = true;
        }
    }
    return r;
}

/* ---------------------------------------------------------------- the walk */

WalkRecorder::WalkRecorder(int weak_dbm, int interval_ms, int hysteresis_ms, std::size_t max_samples)
    : weak_dbm_(weak_dbm), interval_ms_(std::max(1, interval_ms)), hysteresis_ms_(std::max(0, hysteresis_ms)),
      max_samples_(std::max<std::size_t>(1, max_samples))
{
}

void WalkRecorder::add(std::int64_t t, const WalkReading &r)
{
    WalkSample s;
    s.t_ms = t;
    s.linked = r.linked;
    s.dbm = r.linked ? r.dbm : 0;
    s.weak = !r.linked || r.dbm < weak_dbm_;
    ring_.push_back(s);
    while (ring_.size() > max_samples_) {
        ring_.pop_front();
        ++dropped_;
    }
    if (first_t_ < 0) first_t_ = t;

    ++sum_.samples;
    if (r.linked) {
        ++sum_.linked_samples;
        dbm_total_ += r.dbm;
        if (!sum_.has_signal || r.dbm < sum_.weakest_dbm) {
            sum_.weakest_dbm = r.dbm;
            sum_.weakest_at_s = static_cast<double>(t) / 1000.0;
        }
        if (!sum_.has_signal || r.dbm > sum_.strongest_dbm) {
            sum_.strongest_dbm = r.dbm;
            sum_.strongest_at_s = static_cast<double>(t) / 1000.0;
        }
        sum_.has_signal = true;
        sum_.average_dbm = dbm_total_ / sum_.linked_samples;
    } else {
        ++sum_.nolink_samples;
    }
    if (s.weak) ++sum_.weak_samples;
    sum_.duration_s = static_cast<double>(t - first_t_ + interval_ms_) / 1000.0;
    sum_.weak_s = sum_.weak_samples * interval_ms_ / 1000.0;
    sum_.nolink_s = sum_.nolink_samples * interval_ms_ / 1000.0;
    sum_.weak_pct = 100.0 * sum_.weak_samples / sum_.samples;

    /* the dips, with the hysteresis on the way out */
    if (s.weak) {
        if (!in_dip_) {
            in_dip_ = true;
            ++sum_.dips;
            open_dip_ = -1;
            if (dips_.size() < kWalkMaxEvents) {
                WalkDip d;
                d.start_ms = t;
                dips_.push_back(d);
                open_dip_ = static_cast<int>(dips_.size()) - 1;
            }
        }
        above_since_ = -1;
        if (open_dip_ >= 0) {
            WalkDip &d = dips_[static_cast<std::size_t>(open_dip_)];
            if (!r.linked) d.lost_link = true;
            else if (!d.has_dbm || r.dbm < d.lowest_dbm) {
                d.has_dbm = true;
                d.lowest_dbm = r.dbm;
            }
        }
    } else if (in_dip_) {
        if (above_since_ < 0) above_since_ = t;
        if (t - above_since_ + interval_ms_ >= hysteresis_ms_) {
            in_dip_ = false;
            if (open_dip_ >= 0) dips_[static_cast<std::size_t>(open_dip_)].end_ms = above_since_;
            open_dip_ = -1;
            above_since_ = -1;
        }
    }

    /* the access point */
    if (r.has_link && !r.bssid.empty()) {
        sum_.roaming_known = true;
        if (!bssid_.empty() && r.bssid != bssid_) {
            ++sum_.roams;
            if (roams_.size() < kWalkMaxEvents) {
                WalkRoam e;
                e.t_ms = t;
                e.from = bssid_;
                e.to = r.bssid;
                e.ssid = r.ssid;
                e.has_dbm = r.linked;
                e.dbm = r.linked ? r.dbm : 0;
                roams_.push_back(e);
            }
        }
        bssid_ = r.bssid;
        ssid_ = r.ssid;
    }
}

/* ---------------------------------------------------------------- the sampler */

namespace {

WalkConfig clamp_config(const WalkConfig &in)
{
    WalkConfig c = in;
    c.weak_dbm = std::min(kWalkWeakMaxDbm, std::max(kWalkWeakMinDbm, c.weak_dbm));
    c.interval_ms = std::min(5000, std::max(500, c.interval_ms));
    c.hysteresis_ms = std::min(10000, std::max(0, c.hysteresis_ms));
    c.max_samples = std::min<std::size_t>(3600, std::max<std::size_t>(10, c.max_samples));
    if (c.iw.empty() || c.iw[0].empty()) c.iw = {kIwPath};
    return c;
}

WalkSnapshot first_snapshot(const WalkConfig &c, WalkSnapshot::State state)
{
    WalkSnapshot s;
    s.state = state;
    s.iface = c.iface;
    s.weak_dbm = c.weak_dbm;
    s.interval_ms = c.interval_ms;
    s.hysteresis_ms = c.hysteresis_ms;
    return s;
}

/* a command runner that puts the configured iw program in front of iw_link_argv()'s arguments */
class IwProgram : public CommandRunner {
public:
    IwProgram(CommandRunner &inner, const std::vector<std::string> &program) : inner_(inner), program_(program) {}
    CommandResult run(const CommandSpec &spec, const CancelToken &cancel) override
    {
        CommandSpec s = spec;
        if (!s.argv.empty()) {
            s.argv.erase(s.argv.begin());
            s.argv.insert(s.argv.begin(), program_.begin(), program_.end());
        }
        return inner_.run(s, cancel);
    }

private:
    CommandRunner &inner_;
    std::vector<std::string> program_;
};

} // namespace

WalkSnapshot run_walk(const WalkConfig &input, const Source &src, CommandRunner &cmd, const LoopClock &clock,
                      const CancelToken &cancel, const std::function<void(const WalkSnapshot &)> &publish)
{
    const WalkConfig cfg = clamp_config(input);
    WalkSnapshot snap = first_snapshot(cfg, WalkSnapshot::State::Running);
    const auto emit = [&] {
        if (publish) publish(snap);
    };
    if (!iface_is_wireless(src, cfg.iface)) {
        snap.state = WalkSnapshot::State::Failed;
        snap.error = cfg.iface.empty() ? std::string("No Wi-Fi adapter on this board") : "No Wi-Fi adapter on " + printable(cfg.iface);
        emit();
        return snap;
    }
    emit();

    IwProgram iw(cmd, cfg.iw);
    WalkRecorder rec(cfg.weak_dbm, cfg.interval_ms, cfg.hysteresis_ms, cfg.max_samples);
    bool iw_missing = false;
    const std::int64_t start = clock.now();
    std::int64_t tick = 0;
    int taken = 0;
    for (;;) {
        if (cancel.cancelled()) break;
        const WalkReading r = read_walk_signal(src, iw, cfg.iface, cfg.ask_iw, cancel, &iw_missing);
        if (cancel.cancelled()) break;
        const std::int64_t now = clock.now();
        rec.add(tick * cfg.interval_ms, r);
        snap.samples.assign(rec.samples().begin(), rec.samples().end());
        snap.dropped = rec.dropped();
        snap.summary = rec.summary();
        snap.dips = rec.dips();
        snap.roams = rec.roams();
        snap.in_dip = rec.in_dip();
        snap.linked = r.linked;
        snap.dbm = r.linked ? r.dbm : 0;
        snap.bssid = r.has_link ? r.bssid : rec.bssid();
        snap.ssid = r.has_link ? r.ssid : rec.ssid();
        snap.source = !r.linked ? "" : r.from_iw ? "iw" : "/proc/net/wireless";
        snap.iw_missing = iw_missing;
        snap.elapsed_s = static_cast<double>(now - start) / 1000.0;
        emit();
        if (cfg.max_samples_taken > 0 && ++taken >= cfg.max_samples_taken) break;
        /* the next tick on the schedule; ticks already past are skipped */
        std::int64_t next_tick = tick + 1;
        const std::int64_t due = (clock.now() - start) / cfg.interval_ms;
        if (due >= next_tick) next_tick = due + 1;
        tick = next_tick;
        if (!wait_until(clock, start + tick * cfg.interval_ms, cancel, nullptr)) break;
    }
    snap.state = WalkSnapshot::State::Stopped;
    snap.elapsed_s = static_cast<double>(clock.now() - start) / 1000.0;
    emit();
    return snap;
}

WalkSession::WalkSession()
    : src_(make_system_source()), cmd_(std::make_shared<DirectRunner>()), clock_(real_loop_clock())
{
}

WalkSession::WalkSession(std::shared_ptr<const Source> src, std::shared_ptr<CommandRunner> cmd, LoopClock clock)
    : src_(std::move(src)), cmd_(std::move(cmd)), clock_(std::move(clock))
{
}

bool WalkSession::start(const WalkConfig &cfg)
{
    worker_.cancel();
    last_ = clamp_config(cfg);
    const std::uint64_t gen = worker_.generation() + 1;
    WalkSnapshot first = first_snapshot(last_, WalkSnapshot::State::Running);
    channel_.begin(gen, first);
    const std::shared_ptr<const Source> src = src_;
    const std::shared_ptr<CommandRunner> cmd = cmd_;
    const LoopClock clock = clock_;
    const WalkConfig c = last_;
    const std::uint64_t started = worker_.start([this, gen, c, src, cmd, clock](const CancelToken &cancel) {
        run_walk(c, *src, *cmd, clock, cancel, [this, gen](const WalkSnapshot &s) { channel_.publish(gen, s); });
    });
    if (!started) {
        first.state = WalkSnapshot::State::Failed;
        first.error = "Could not start the walk test (no thread).";
        channel_.begin(gen, first);
        return false;
    }
    return true;
}

void WalkSession::stop()
{
    worker_.cancel();
}

void WalkSession::reset()
{
    worker_.cancel();
    /* a generation no run has: the stopping run's last snapshots are dropped */
    channel_.begin(worker_.generation() + 1, first_snapshot(last_, WalkSnapshot::State::Idle));
}

} // namespace nt
