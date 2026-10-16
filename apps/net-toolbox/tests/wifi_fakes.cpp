/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the fakes of the Wi-Fi scan and the walk test (see wifi_fakes.hpp).
 */
#include "wifi_fakes.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace wfake {

nt::CommandResult result(int exit_code, const std::string &output)
{
    nt::CommandResult r;
    r.started = true;
    r.exit_code = exit_code;
    r.output = output;
    return r;
}

nt::CommandResult not_installed()
{
    nt::CommandResult r;
    r.started = false;
    r.exit_code = 127;
    return r;
}

nt::LoopClock clock_of(std::shared_ptr<Time> time)
{
    nt::LoopClock c;
    c.now = [time] { return time->t.load(); };
    c.sleep = [time](int ms) {
        if (time->real_us > 0) usleep(static_cast<useconds_t>(time->real_us));
        const std::int64_t now = time->t.fetch_add(ms) + ms;
        if (time->on_sleep) time->on_sleep(now);
    };
    return c;
}

nt::CommandResult Runner::run(const nt::CommandSpec &spec, const nt::CancelToken &cancel)
{
    int index = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        index = static_cast<int>(specs_.size());
        specs_.push_back(spec);
        times_.push_back(time ? time->t.load() : 0);
    }
    if (on_call) on_call(index);
    if (cancel.cancelled()) {
        nt::CommandResult c;
        c.started = true;
        c.cancelled = true;
        return c;
    }
    return answer ? answer(spec, index) : result(0, "");
}

int Runner::calls() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(specs_.size());
}

nt::CommandSpec Runner::spec(int i) const
{
    std::lock_guard<std::mutex> lock(mu_);
    return i >= 0 && i < static_cast<int>(specs_.size()) ? specs_[static_cast<std::size_t>(i)] : nt::CommandSpec();
}

std::int64_t Runner::at(int i) const
{
    std::lock_guard<std::mutex> lock(mu_);
    return i >= 0 && i < static_cast<int>(times_.size()) ? times_[static_cast<std::size_t>(i)] : -1;
}

std::string Runner::rescan(int i) const
{
    const nt::CommandSpec s = spec(i);
    for (std::size_t k = 0; k + 1 < s.argv.size(); ++k)
        if (s.argv[k] == "--rescan") return s.argv[k + 1];
    return "";
}

ScriptSource::ScriptSource(const std::string &fixture_dir, std::vector<std::string> proc)
    : base_(nt::make_fixture_source(fixture_dir)), proc_(std::move(proc))
{
}

std::string ScriptSource::read(const std::string &path, bool *ok) const
{
    if (path == "/proc/net/wireless") {
        std::lock_guard<std::mutex> lock(mu_);
        if (ok) *ok = !proc_.empty();
        if (proc_.empty()) return "";
        const std::string &text = proc_[std::min(next_, proc_.size() - 1)];
        ++next_;
        return text;
    }
    return base_->read(path, ok);
}

bool ScriptSource::exists(const std::string &path) const
{
    return base_->exists(path);
}

std::vector<std::string> ScriptSource::list(const std::string &dir) const
{
    return base_->list(dir);
}

std::vector<nt::IfAddr> ScriptSource::addresses() const
{
    return base_->addresses();
}

int ScriptSource::proc_reads() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(next_);
}

std::string proc_wireless(const std::vector<std::pair<std::string, int>> &levels)
{
    std::string out = "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
                      " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n";
    for (const auto &l : levels) {
        char line[160];
        std::snprintf(line, sizeof(line), " %s: 0000   %d.  %d.  -256        0      0      0      3      0        0\n",
                      l.first.c_str(), l.second < 0 ? l.second + 110 : 0, l.second);
        out += line;
    }
    return out;
}

} // namespace wfake
