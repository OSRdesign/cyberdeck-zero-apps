/*
 * SPDX-License-Identifier: MIT
 */

#include "deck_clock.hpp"

#include <cstdio>
#include <sys/wait.h>

namespace meshzero {

namespace {

bool run_command(const char *cmd, std::string &out)
{
    out.clear();
    FILE *p = ::popen(cmd, "r");
    if (!p) return false;
    char buf[128];
    while (std::fgets(buf, sizeof(buf), p)) {
        out += buf;
        if (out.size() > 512) break;
    }
    const int status = ::pclose(p);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string first_word(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\n' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = a;
    while (b < s.size() && s[b] != ' ' && s[b] != '\n' && s[b] != '\t' && s[b] != '\r') ++b;
    return s.substr(a, b - a);
}

} // namespace

DeckClock parse_ntp_output(const std::string &out, bool ok)
{
    if (!ok) return DeckClock::Unknown;
    const std::string w = first_word(out);
    if (w == "yes") return DeckClock::Synced;
    if (w == "no") return DeckClock::Unsynced;
    return DeckClock::Unknown;
}

DeckClock parse_connectivity_output(const std::string &out, bool ok)
{
    if (!ok) return DeckClock::Unknown;
    const std::string w = first_word(out);
    if (w == "full") return DeckClock::Synced;      // a hint only: the Internet is reachable, so NTP very probably works
    if (w == "none" || w == "limited" || w == "portal") return DeckClock::Unsynced;
    return DeckClock::Unknown;
}

DeckClockProbe::~DeckClockProbe()
{
    if (worker_.joinable()) worker_.join();
}

void DeckClockProbe::refresh()
{
    if (running_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    result_ = 0;
    worker_ = std::thread([this] { run(); });
}

void DeckClockProbe::run()
{
    std::string out;
    DeckClock st = parse_ntp_output(out, run_command("timedatectl show -p NTPSynchronized --value 2>/dev/null", out));
    if (st == DeckClock::Unknown) st = parse_connectivity_output(out, run_command("nmcli -t -g CONNECTIVITY general 2>/dev/null", out));
    result_ = st == DeckClock::Synced ? 1 : st == DeckClock::Unsynced ? 2 : 3;
    running_ = false;
}

DeckClock DeckClockProbe::state() const
{
    switch (result_.load()) {
    case 1: return DeckClock::Synced;
    case 2: return DeckClock::Unsynced;
    default: return DeckClock::Unknown;
    }
}

} // namespace meshzero
