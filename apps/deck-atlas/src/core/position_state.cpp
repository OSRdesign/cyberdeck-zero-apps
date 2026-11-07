/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: position.state (see position_state.hpp).
 */
#include "position_state.hpp"

#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace atlas {

namespace {

std::string num(const char *fmt, double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

} // namespace

std::string format_position_state(const GpsStatus &status, const PositionStateInfo &info)
{
    std::string out = "v=1\n";
    out += "gen=" + std::to_string(info.gen) + "\n";
    out += "pid=" + std::to_string(info.pid) + "\n";
    out += "writer=" + info.writer + "\n";
    const bool fixed = status.has_fix() && !status.stale;
    if (!fixed) {
        out += "fix=0\n";
        out += "mono_ms=" + std::to_string(info.now_ms) + "\n";
        if (!info.source.empty()) out += "source=" + info.source + "\n";
        return out;
    }
    const Fix &f = status.fix;
    out += std::string("fix=") + (status.kind == GpsStatus::Kind::Fix3D ? "3" : "2") + "\n";
    out += "lat=" + num("%.6f", f.lat) + "\n";
    out += "lon=" + num("%.6f", f.lon) + "\n";
    if (f.has_alt && status.kind == GpsStatus::Kind::Fix3D) out += "alt_m=" + num("%.1f", f.alt_m) + "\n";
    if (f.speed_mps >= 0) out += "speed_mps=" + num("%.2f", f.speed_mps) + "\n";
    if (f.course_deg >= 0) out += "course_deg=" + num("%.1f", f.course_deg) + "\n";
    out += "acc_m=" + num("%.1f", accuracy_from_hdop(f.hdop)) + "\n";
    if (f.hdop >= 0) out += "hdop=" + num("%.2f", f.hdop) + "\n";
    if (f.sats_used >= 0) out += "sats_used=" + std::to_string(f.sats_used) + "\n";
    if (f.sats_view >= 0) out += "sats_view=" + std::to_string(f.sats_view) + "\n";
    if (!f.utc.empty()) out += "utc=" + f.utc + "\n";
    out += "mono_ms=" + std::to_string(f.fix_ms) + "\n";
    if (!info.source.empty()) out += "source=" + info.source + "\n";
    return out;
}

std::string position_state_dir()
{
    if (const char *d = std::getenv("APPLAUNCH_STATE_DIR"); d && *d) return d;
    if (const char *x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/applaunch";
    return std::string();
}

PositionStateWriter::PositionStateWriter(std::string dir, int pid, WriteFn write)
    : dir_(std::move(dir)), pid_(pid), write_(std::move(write))
{
    if (!dir_.empty()) path_ = dir_ + "/position.state";
    if (!write_) write_ = [](const std::string &p, const std::string &d) { return write_file_atomic(p, d); };
}

void PositionStateWriter::update(const GpsStatus &status, const std::string &source, std::int64_t now_ms)
{
    if (dir_.empty()) return;
    if (status.kind == GpsStatus::Kind::Off) {
        clear();
        return;
    }
    PositionStateInfo info;
    info.pid = pid_;
    info.gen = 0;
    info.source = source;
    info.now_ms = 0;
    const std::string sig = format_position_state(status, info); /* the content without gen and the write's time */
    if (wrote_ && sig == last_sig_) return;
    if (tried_ && now_ms - last_write_ms_ < 1000) return; /* at most once a second: the next tick tries again */
    info.gen = ++gen_;
    info.now_ms = now_ms;
    make_dirs(dir_);
    tried_ = true;
    last_write_ms_ = now_ms;
    if (write_(path_, format_position_state(status, info))) {
        wrote_ = true;
        last_sig_ = sig;
        ++writes_;
    } else {
        wrote_ = false; /* tried again a second later */
    }
}

void PositionStateWriter::clear()
{
    if (dir_.empty()) return;
    if (wrote_) ::unlink(path_.c_str());
    wrote_ = false;
    last_sig_.clear();
}

} // namespace atlas
