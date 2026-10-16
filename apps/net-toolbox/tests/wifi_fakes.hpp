/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.4): the fakes of the Wi-Fi scan and the walk test: a command runner that answers from a function
 * and logs what it was asked and when (thread-safe: the sessions call it on their worker), a loop clock that moves at
 * once (optionally with a short real sleep, for the sessions), and a Source whose /proc/net/wireless is a script.
 */
#pragma once

#include "cmdrun.hpp"
#include "source.hpp"
#include "wifiscan.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfake {

/* a result: started, the exit code, the output */
nt::CommandResult result(int exit_code, const std::string &output);
nt::CommandResult not_installed();

/* the fake time of the loops: starts at `t`, sleep(ms) adds ms (after real_us of real sleep, when set) */
struct Time {
    std::atomic<std::int64_t> t{1000};
    int real_us = 0;
    std::function<void(std::int64_t now)> on_sleep; /* after each sleep (e.g. set the rescan flag, cancel) */
};
nt::LoopClock clock_of(std::shared_ptr<Time> time);

class Runner : public nt::CommandRunner {
public:
    /* index = the call's number (0 first) */
    std::function<nt::CommandResult(const nt::CommandSpec &spec, int index)> answer;
    std::function<void(int index)> on_call;  /* before answering (advance the clock, cancel ...) */
    std::shared_ptr<Time> time;              /* when set, each call's time is logged */
    nt::CommandResult run(const nt::CommandSpec &spec, const nt::CancelToken &cancel) override;

    int calls() const;
    nt::CommandSpec spec(int i) const;
    std::int64_t at(int i) const;
    /* the value after "--rescan" in call i ("" none) */
    std::string rescan(int i) const;

private:
    mutable std::mutex mu_;
    std::vector<nt::CommandSpec> specs_;
    std::vector<std::int64_t> times_;
};

/* /proc/net/wireless from a list (each read takes the next; the last one stays), the rest from a fixture folder */
class ScriptSource : public nt::Source {
public:
    ScriptSource(const std::string &fixture_dir, std::vector<std::string> proc);
    std::string read(const std::string &path, bool *ok = nullptr) const override;
    bool exists(const std::string &path) const override;
    std::vector<std::string> list(const std::string &dir) const override;
    std::vector<nt::IfAddr> addresses() const override;
    std::string describe() const override { return "script"; }
    int proc_reads() const;

private:
    std::unique_ptr<nt::Source> base_;
    std::vector<std::string> proc_;
    mutable std::mutex mu_;
    mutable std::size_t next_ = 0;
};

/* /proc/net/wireless with one line per (interface, level) */
std::string proc_wireless(const std::vector<std::pair<std::string, int>> &levels);

} // namespace wfake
