/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the sky the receiver reports (see gps_sky.hpp).
 */
#include "gps_sky.hpp"

#include <algorithm>
#include <cstring>

namespace atlas {

namespace {

bool is_sbas_prn(int prn) { return (prn >= 33 && prn <= 64) || (prn >= 120 && prn <= 158); }

Constellation from_system_id(int id)
{
    switch (id) {
    case 1: return Constellation::Gps;
    case 2: return Constellation::Glonass;
    case 3: return Constellation::Galileo;
    case 4: return Constellation::Beidou;
    case 5: return Constellation::Qzss;
    default: return Constellation::Unknown;
    }
}

Constellation from_talker(const char talker[3])
{
    if (talker[0] != 'G') return Constellation::Unknown;
    switch (talker[1]) {
    case 'P': return Constellation::Gps;
    case 'L': return Constellation::Glonass;
    case 'A': return Constellation::Galileo;
    case 'B': return Constellation::Beidou;
    case 'Q': return Constellation::Qzss;
    default: return Constellation::Unknown;
    }
}

Constellation from_prn_range(int prn)
{
    if (prn >= 1 && prn <= 32) return Constellation::Gps;
    if (is_sbas_prn(prn)) return Constellation::Sbas;
    if (prn >= 65 && prn <= 96) return Constellation::Glonass;
    if (prn >= 193 && prn <= 202) return Constellation::Qzss;
    if (prn >= 301 && prn <= 336) return Constellation::Galileo;
    if (prn >= 401 && prn <= 437) return Constellation::Beidou;
    return Constellation::Unknown;
}

bool talker_bd(const char talker[3]) { return talker[0] == 'B' && talker[1] == 'D'; }

int order_of(Constellation c)
{
    switch (c) {
    case Constellation::Gps: return 0;
    case Constellation::Glonass: return 1;
    case Constellation::Galileo: return 2;
    case Constellation::Beidou: return 3;
    case Constellation::Qzss: return 4;
    case Constellation::Sbas: return 5;
    default: return 6;
    }
}

std::string talker_str(const char talker[3]) { return std::string(talker, talker[0] ? (talker[1] ? 2 : 1) : 0); }

} // namespace

char constellation_letter(Constellation c)
{
    switch (c) {
    case Constellation::Gps: return 'G';
    case Constellation::Glonass: return 'R';
    case Constellation::Galileo: return 'E';
    case Constellation::Beidou: return 'C';
    case Constellation::Qzss: return 'J';
    case Constellation::Sbas: return 'S';
    default: return '?';
    }
}

const char *constellation_name(Constellation c)
{
    switch (c) {
    case Constellation::Gps: return "GPS";
    case Constellation::Glonass: return "GLONASS";
    case Constellation::Galileo: return "Galileo";
    case Constellation::Beidou: return "BeiDou";
    case Constellation::Qzss: return "QZSS";
    case Constellation::Sbas: return "SBAS";
    default: return "Unknown";
    }
}

Constellation constellation_of(const char talker[3], int prn, int system_id)
{
    Constellation base = from_system_id(system_id);
    if (base == Constellation::Unknown) {
        if (talker_bd(talker)) base = Constellation::Beidou;
        else base = from_talker(talker);
    }
    if (base == Constellation::Unknown && talker[0] == 'G' && talker[1] == 'N') return from_prn_range(prn);
    if (base == Constellation::Gps && is_sbas_prn(prn)) return Constellation::Sbas;
    return base;
}

void SkyTracker::reset()
{
    table_.clear();
    groups_.clear();
    gsa_.clear();
}

void SkyTracker::commit(const std::string &talker, const Group &g, std::int64_t now_ms)
{
    char t[3] = {talker.size() > 0 ? talker[0] : '\0', talker.size() > 1 ? talker[1] : '\0', '\0'};
    for (const GsvSat &s : g.sats) {
        const Constellation sys = constellation_of(t, s.prn);
        SkySat &e = table_[{static_cast<int>(sys), s.prn}];
        const bool fresh_other_signal = e.seen_ms != 0 && now_ms - e.seen_ms <= 2000 && e.signal != g.signal;
        e.sys = sys;
        e.prn = s.prn;
        if (s.elev >= 0) e.elev = s.elev;
        if (s.azim >= 0) e.azim = s.azim;
        /* a second signal of the same satellite heard within 2 s: keep the stronger SNR */
        if (!(fresh_other_signal && e.snr >= 0 && s.snr < e.snr)) {
            e.snr = s.snr;
            e.signal = g.signal;
        }
        e.seen_ms = now_ms;
    }
}

void SkyTracker::on_msg(const NmeaMsg &m, std::int64_t now_ms)
{
    if (m.kind == MsgKind::Gsv && m.gsv_total > 0) {
        const std::string key = talker_str(m.talker) + ":" + std::to_string(m.gsv_signal);
        Group &g = groups_[key];
        if (m.gsv_num == 1) {
            g = Group();
            g.total = m.gsv_total;
            g.signal = m.gsv_signal;
        } else if (g.total != m.gsv_total || g.next != m.gsv_num) {
            groups_.erase(key); /* a message was lost or the group began unheard: drop what was gathered */
            return;
        }
        g.sats.insert(g.sats.end(), m.gsv_sats.begin(), m.gsv_sats.end());
        g.next = m.gsv_num + 1;
        if (m.gsv_num == m.gsv_total) {
            commit(talker_str(m.talker), g, now_ms);
            groups_.erase(key);
        }
        for (auto it = table_.begin(); it != table_.end();)
            if (now_ms - it->second.seen_ms > kSatKeepMs) it = table_.erase(it);
            else ++it;
    } else if (m.kind == MsgKind::Gsa) {
        GsaReport r;
        r.talker[0] = m.talker[0];
        r.talker[1] = m.talker[1];
        r.system_id = m.system_id;
        r.sys = constellation_of(m.talker, 0, m.system_id);
        r.mode = m.mode;
        r.manual = m.gsa_manual;
        r.used = m.used_prns;
        if (m.has_pdop) r.pdop = m.pdop;
        if (m.has_hdop) r.hdop = m.hdop;
        if (m.has_vdop) r.vdop = m.vdop;
        r.ms = now_ms;
        gsa_[talker_str(m.talker) + ":" + std::to_string(m.system_id)] = r;
    }
}

std::vector<SkySat> SkyTracker::satellites(std::int64_t now_ms) const
{
    std::vector<SkySat> out;
    for (const auto &kv : table_) {
        if (now_ms - kv.second.seen_ms > kSatKeepMs) continue;
        SkySat s = kv.second;
        s.used = false;
        for (const auto &g : gsa_) {
            const GsaReport &r = g.second;
            if (now_ms - r.ms > kGsaFreshMs || r.mode < 2) continue;
            for (int prn : r.used) {
                if (constellation_of(r.talker, prn, r.system_id) == s.sys && (prn == s.prn)) s.used = true;
                /* some receivers number GLONASS 1-32 in a GSA and 65-96 in a GSV */
                if (s.sys == Constellation::Glonass && prn <= 32 && prn + 64 == s.prn &&
                    constellation_of(r.talker, prn, r.system_id) == Constellation::Glonass)
                    s.used = true;
            }
        }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const SkySat &a, const SkySat &b) {
        if (order_of(a.sys) != order_of(b.sys)) return order_of(a.sys) < order_of(b.sys);
        return a.prn < b.prn;
    });
    return out;
}

std::vector<GsaReport> SkyTracker::reports(std::int64_t now_ms) const
{
    std::vector<GsaReport> out;
    for (const auto &kv : gsa_)
        if (now_ms - kv.second.ms <= kGsaFreshMs) out.push_back(kv.second);
    return out; /* std::map order: talker then system id */
}

Dop SkyTracker::dop(std::int64_t now_ms) const
{
    Dop d;
    std::int64_t best = -1;
    for (const auto &kv : gsa_) {
        const GsaReport &r = kv.second;
        if (now_ms - r.ms > kGsaFreshMs || r.ms < best) continue;
        if (r.pdop < 0 && r.hdop < 0 && r.vdop < 0) continue;
        best = r.ms;
        d.pdop = r.pdop;
        d.hdop = r.hdop;
        d.vdop = r.vdop;
    }
    return d;
}

} // namespace atlas
