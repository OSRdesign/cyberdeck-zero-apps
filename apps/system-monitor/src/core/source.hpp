/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: where the sampler reads from. The system (/proc, /sys, statvfs, getifaddrs, commands), or a fixture
 * folder for the tests and the headless scenes.
 *
 * A fixture folder mirrors the system: <dir>/proc/stat, <dir>/sys/class/thermal/thermal_zone0/temp, ... and adds
 *   cmd/<argv joined by _>   a command's output (cmd/vcgencmd_get_throttled); a first line "exit=N" sets its status;
 *                            no file = the command is not installed
 *   statvfs                  "<mount point> <total bytes> <used bytes> <available bytes>" per line
 *   ipv4                     "<interface> <address>/<prefix>" per line
 *   rootdev                  "<major>:<minor>" of the root filesystem
 *   pagesize                 bytes (default 4096)
 * and, for the scenes, a scenario (scenario-<name>.conf, SYSTEM_MONITOR_FIXTURE=<folder>:<name>) that makes the
 * counters move from sample to sample in virtual time (one sample = one period): the CPU load of each core (a slow
 * wave around a mean), the temperature (a ramp), the power flags (sample ranges), network and disk rates, memory use
 * and the processes. See FixtureSource in source.cpp for the keys.
 */
#pragma once

#include "command.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sm {

class Source {
public:
    virtual ~Source() = default;
    /* the whole file (capped at 1 MB), "" and ok = false when unreadable */
    virtual std::string read(const std::string &path, bool *ok = nullptr) = 0;
    virtual bool exists(const std::string &path) = 0;
    virtual std::vector<std::string> list(const std::string &dir) = 0; /* names, sorted, without . and .. */
    virtual CmdResult run(const std::vector<std::string> &argv, int timeout_ms) = 0;
    /* statvfs of a mount point, bytes (used as df counts it: total - free) */
    virtual bool fs_usage(const std::string &dir, std::uint64_t &total, std::uint64_t &used, std::uint64_t &avail) = 0;
    virtual std::map<std::string, std::string> ipv4() = 0; /* interface -> "192.168.1.23/24" (the first address) */
    virtual bool root_dev(unsigned &major, unsigned &minor) = 0;
    virtual long page_size() = 0;
    virtual std::int64_t now_ms() = 0; /* monotonic */
    /* a sample begins (a fixture with a scenario moves its counters one period on) */
    virtual void begin_sample(int period_ms) { (void)period_ms; }
    /* time is the scenario's: the sampler fills the history at once instead of waiting */
    virtual bool virtual_time() const { return false; }
    virtual std::string describe() const = 0;
};

std::unique_ptr<Source> make_system_source();
/* dir: a fixture folder; scenario: "" or the name of its scenario-<name>.conf */
std::unique_ptr<Source> make_fixture_source(const std::string &dir, const std::string &scenario);

/* SYSTEM_MONITOR_FIXTURE=<folder>[:<scenario>]: an absolute folder, or a name under the app's tests/fixtures (host
 * builds). The source, or nullptr when the variable is unset (the system). */
std::unique_ptr<Source> source_from_env(std::string *described = nullptr);

} // namespace sm
