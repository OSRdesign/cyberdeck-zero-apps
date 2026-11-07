/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the position the receiver reports (see gps_fix.hpp).
 */
#include "gps_fix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace atlas {

namespace {
constexpr std::int64_t kModeFreshMs = 3000;   /* a GSA / GSV counts for this long */
constexpr std::int64_t kInvalidIgnoreMs = 1500; /* an invalid RMC next to a valid GGA of the same second is no loss */
} // namespace

void FixTracker::reset()
{
    fix_ = Fix();
    gsa_.clear();
    gsv_.clear();
    alt_known_ = false;
    sky_.reset();
}

int FixTracker::mode_now(std::int64_t now_ms) const
{
    int best = 0;
    for (const auto &kv : gsa_)
        if (now_ms - kv.second.ms <= kModeFreshMs && kv.second.value > best) best = kv.second.value;
    return best;
}

int FixTracker::in_view_now(std::int64_t now_ms) const
{
    int sum = 0;
    bool any = false;
    for (const auto &kv : gsv_)
        if (now_ms - kv.second.ms <= kModeFreshMs) {
            sum += kv.second.value;
            any = true;
        }
    return any ? sum : -1;
}

void FixTracker::set_fix(const NmeaMsg &m, std::int64_t now_ms)
{
    fix_.lat = m.lat;
    fix_.lon = m.lon;
    fix_.fix_ms = now_ms;
    const int mode = mode_now(now_ms);
    fix_.kind = mode == 3 ? 3 : mode == 2 ? 2 : (alt_known_ ? 3 : 2);
}

void FixTracker::on_msg(const NmeaMsg &m, std::int64_t now_ms)
{
    sky_.on_msg(m, now_ms);
    switch (m.kind) {
    case MsgKind::Gsa:
        if (m.mode > 0) gsa_[std::string(m.talker) + ':' + std::to_string(m.system_id)] = Seen{m.mode, now_ms};
        if (m.has_hdop) fix_.hdop = m.hdop;
        break;
    case MsgKind::Gsv:
        if (m.has_in_view) gsv_[m.talker] = Seen{m.in_view, now_ms};
        fix_.sats_view = in_view_now(now_ms);
        break;
    case MsgKind::Gga:
        if (m.has_sats) fix_.sats_used = m.sats;
        if (m.has_hdop) fix_.hdop = m.hdop;
        if (m.valid && m.has_pos) {
            if (m.has_alt) {
                alt_known_ = true;
                fix_.has_alt = true;
                fix_.alt_m = m.alt_m;
            }
            set_fix(m, now_ms);
        } else if (!m.valid && now_ms - fix_.fix_ms > kInvalidIgnoreMs) {
            fix_.kind = 0;
            fix_.speed_mps = fix_.course_deg = -1;
            fix_.has_alt = false;
            alt_known_ = false;
        }
        break;
    case MsgKind::Rmc:
        if (m.valid && m.has_pos) {
            set_fix(m, now_ms);
            fix_.speed_mps = m.has_speed ? m.speed_mps : -1;
            fix_.course_deg = m.has_course ? m.course_deg : -1;
            if (m.has_date && m.has_time) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", m.year, m.month, m.day, m.hour, m.minute,
                              static_cast<int>(m.second));
                fix_.utc = buf;
            }
        } else if (!m.valid && now_ms - fix_.fix_ms > kInvalidIgnoreMs) {
            fix_.kind = 0;
            fix_.speed_mps = fix_.course_deg = -1;
            fix_.has_alt = false;
            alt_known_ = false;
        }
        break;
    default: break;
    }
}

bool receiver_alive(const GpsLinkFacts &f, std::int64_t now_ms)
{
    return !f.port.empty() && f.last_valid_ms > 0 && now_ms - f.last_valid_ms <= kReceiverAliveMs;
}

double accuracy_from_hdop(double hdop)
{
    if (!(hdop > 0)) return 10.0;
    return std::min(100.0, std::max(3.0, hdop * 5.0));
}

GpsStatus make_status(GpsLinkState link, const FixTracker &tracker, std::int64_t now_ms)
{
    GpsStatus s;
    switch (link) {
    case GpsLinkState::Off: s.kind = GpsStatus::Kind::Off; return s;
    case GpsLinkState::NoDevice: s.kind = GpsStatus::Kind::NoDevice; return s;
    case GpsLinkState::NoReceiver: s.kind = GpsStatus::Kind::NoReceiver; return s;
    case GpsLinkState::NoAccess: s.kind = GpsStatus::Kind::NoAccess; return s;
    case GpsLinkState::Searching: s.kind = GpsStatus::Kind::Searching; return s;
    case GpsLinkState::Locked: break;
    }
    s.sats = tracker.sky().satellites(now_ms);
    s.gsa = tracker.sky().reports(now_ms);
    s.dop = tracker.sky().dop(now_ms);
    const Fix &f = tracker.fix();
    if (f.kind == 0) {
        s.kind = GpsStatus::Kind::Searching;
        return s;
    }
    s.kind = f.kind == 3 ? GpsStatus::Kind::Fix3D : GpsStatus::Kind::Fix2D;
    s.fix = f;
    s.age_ms = std::max<std::int64_t>(0, now_ms - f.fix_ms);
    s.stale = s.age_ms > kStaleMs;
    return s;
}

std::string chip_text(const GpsStatus &s, bool miles)
{
    switch (s.kind) {
    case GpsStatus::Kind::Off: return "";
    case GpsStatus::Kind::NoDevice: return "No GPS device";
    case GpsStatus::Kind::NoReceiver: return "No GPS receiver found";
    case GpsStatus::Kind::NoAccess: return "No access to GPS port";
    case GpsStatus::Kind::Searching: return "Searching";
    default: break;
    }
    if (s.stale) {
        const long secs = static_cast<long>(s.age_ms / 1000);
        char buf[48];
        if (secs < 60) std::snprintf(buf, sizeof buf, "Last fix %ld s ago", secs);
        else std::snprintf(buf, sizeof buf, "Last fix %ld min ago", secs / 60);
        return buf;
    }
    std::string t = s.kind == GpsStatus::Kind::Fix3D ? "3D fix" : "2D fix";
    if (s.kind == GpsStatus::Kind::Fix3D && s.fix.sats_used >= 0) t += " " + std::to_string(s.fix.sats_used) + (s.fix.sats_used == 1 ? " sat" : " sats");
    if (s.fix.speed_mps > kMovingMps) {
        const long v = std::lround(s.fix.speed_mps * (miles ? 2.236936 : 3.6));
        t += ", " + std::to_string(v) + (miles ? " mph" : " km/h");
    }
    return t;
}

} // namespace atlas
