/*
 * SPDX-License-Identifier: MIT
 *
 * Net Toolbox tests (0.3): a scripted stream runner (the speed test's curl, the BLE scan's bluetoothctl) on a fake clock,
 * a scripted command runner, and a ping beside the load that returns prepared packets.
 */
#pragma once

#include "cmdrun.hpp"
#include "speedtest.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fake {

/* one moment of a scripted command: its time (the fake clock), the bytes so far, the lines it printed then */
struct Step {
    std::int64_t at_us = 0;
    std::uint64_t out = 0;      /* stdout bytes so far */
    std::uint64_t in = 0;       /* stdin bytes taken so far */
    std::vector<std::string> lines;
};

struct Script {
    std::vector<Step> steps;      /* played in order */
    std::vector<Step> after_end;  /* played once the client closed stdin (then the command exits) */
    int exit_code = 0;            /* when the steps run out */
    int term_signal = 0;
    bool timed_out = false;
    bool started = true;
    bool hang = false;            /* after the steps: tick every tick_us until the client stops it or closes stdin */
    std::int64_t tick_us = 100000;
    std::int64_t hang_limit_us = 600000000; /* then: timed out */
    int real_sleep_us = 0;        /* real time per step (a test with a real ping beside it) */
};

class StreamRunner : public nt::StreamRunner {
public:
    std::vector<Script> scripts;            /* one per run(), in order; missing: exits 0 at once */
    /* what happened, per run() */
    std::vector<nt::StreamSpec> specs;
    std::vector<std::string> stdin_log;     /* text, sends, the farewell when it was written */
    std::vector<bool> input_ended, stopped, cancelled, farewell;
    nt::StreamResult run(const nt::StreamSpec &spec, const nt::CancelToken &cancel, nt::StreamClient &client) override;
};

/* a command runner answering from a list (the BLE `show`) */
class CommandRunner : public nt::CommandRunner {
public:
    std::vector<nt::CommandResult> results;
    std::vector<nt::CommandSpec> specs;
    nt::CommandResult run(const nt::CommandSpec &spec, const nt::CancelToken &cancel) override;
};

/* a ping beside the load: each one made gets the next prepared round trips (-1 = lost) */
struct PingPlan {
    std::vector<std::vector<double>> runs; /* per LoadPing made, in order */
    bool fail_start = false;
    std::string fail_words;
    std::vector<nt::PingConfig> started;   /* what each was asked */
    int made = 0;
    int stopped = 0;
};
nt::LoadPingFactory scripted_ping(std::shared_ptr<PingPlan> plan);

/* a fixture file's lines, as split on '\n' only (the '\r' kept) */
std::vector<std::string> raw_lines(const std::string &text);

} // namespace fake
