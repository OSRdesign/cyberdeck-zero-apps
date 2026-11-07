/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: NMEA 0183, read only (see nmea.hpp).
 */
#include "nmea.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace atlas {

namespace {

constexpr double kKnotMps = 0.514444;

std::vector<std::string> split_fields(const std::string &body)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : body) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool parse_double(const std::string &s, double &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parse_int(const std::string &s, int &out)
{
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    if (s.size() > 6) return false;
    out = std::atoi(s.c_str());
    return true;
}

bool all_digits(const std::string &s, std::size_t a, std::size_t b)
{
    if (a >= b || b > s.size()) return false;
    for (std::size_t i = a; i < b; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* "ddmm.mmmm" / "dddmm.mmmm" and N/S/E/W to signed degrees; false for empty or out of range */
bool parse_coord(const std::string &v, const std::string &hemi, bool is_lat, double &out)
{
    if (v.empty() || hemi.size() != 1) return false;
    const std::size_t dot = v.find('.');
    const std::size_t ip = dot == std::string::npos ? v.size() : dot;
    if (ip < 3) return false;
    const std::size_t mpos = ip - 2;
    if (!all_digits(v, 0, ip)) return false;
    if (dot != std::string::npos && (dot + 1 >= v.size() || !all_digits(v, dot + 1, v.size()))) return false;
    const int deg = std::atoi(v.substr(0, mpos).c_str());
    double minutes = 0;
    if (!parse_double(v.substr(mpos), minutes)) return false;
    if (minutes < 0 || minutes >= 60.0) return false;
    const double d = deg + minutes / 60.0;
    if (d > (is_lat ? 90.0 : 180.0)) return false;
    const char h = hemi[0];
    if (is_lat) {
        if (h != 'N' && h != 'S') return false;
    } else if (h != 'E' && h != 'W') {
        return false;
    }
    out = (h == 'S' || h == 'W') ? -d : d;
    return true;
}

void parse_time(const std::string &v, NmeaMsg &m)
{
    if (v.size() < 6 || !all_digits(v, 0, 6)) return;
    const int hh = std::atoi(v.substr(0, 2).c_str()), mm = std::atoi(v.substr(2, 2).c_str());
    double ss = 0;
    if (!parse_double(v.substr(4), ss)) return;
    if (hh > 23 || mm > 59 || ss >= 61.0) return;
    m.has_time = true;
    m.hour = hh;
    m.minute = mm;
    m.second = ss;
}

const std::string &field(const std::vector<std::string> &f, std::size_t i)
{
    static const std::string kEmpty;
    return i < f.size() ? f[i] : kEmpty;
}

void parse_rmc(const std::vector<std::string> &f, NmeaMsg &m)
{
    parse_time(field(f, 1), m);
    const std::string &mode = field(f, 12); /* NMEA 2.3+: the mode indicator (A, D, E, N...), absent in older receivers */
    m.valid = field(f, 2) == "A" && !(mode == "N");
    const std::string &date = field(f, 9);
    if (date.size() == 6 && all_digits(date, 0, 6)) {
        const int d = std::atoi(date.substr(0, 2).c_str()), mo = std::atoi(date.substr(2, 2).c_str()),
                  y = std::atoi(date.substr(4, 2).c_str());
        if (d >= 1 && d <= 31 && mo >= 1 && mo <= 12) {
            m.has_date = true;
            m.day = d;
            m.month = mo;
            m.year = 2000 + y;
        }
    }
    if (!m.valid) return;
    double lat = 0, lon = 0;
    if (parse_coord(field(f, 3), field(f, 4), true, lat) && parse_coord(field(f, 5), field(f, 6), false, lon)) {
        m.has_pos = true;
        m.lat = lat;
        m.lon = lon;
    } else {
        m.valid = false; /* "A" without a usable position is no fix */
        return;
    }
    double v = 0;
    if (parse_double(field(f, 7), v) && v >= 0 && v < 5000) {
        m.has_speed = true;
        m.speed_mps = v * kKnotMps;
    }
    if (parse_double(field(f, 8), v) && v >= 0 && v <= 360.0) {
        m.has_course = true;
        m.course_deg = v >= 360.0 ? 0.0 : v;
    }
}

void parse_gga(const std::vector<std::string> &f, NmeaMsg &m)
{
    parse_time(field(f, 1), m);
    int q = 0;
    if (parse_int(field(f, 6), q)) m.quality = q;
    m.valid = m.quality > 0;
    int n = 0;
    if (parse_int(field(f, 7), n) && n <= 99) {
        m.has_sats = true;
        m.sats = n;
    }
    double v = 0;
    if (parse_double(field(f, 8), v) && v >= 0 && v < 100) {
        m.has_hdop = true;
        m.hdop = v;
    }
    if (!m.valid) return;
    double lat = 0, lon = 0;
    if (parse_coord(field(f, 2), field(f, 3), true, lat) && parse_coord(field(f, 4), field(f, 5), false, lon)) {
        m.has_pos = true;
        m.lat = lat;
        m.lon = lon;
    } else {
        m.valid = false;
        return;
    }
    if (parse_double(field(f, 9), v) && v > -1000 && v < 100000) {
        m.has_alt = true;
        m.alt_m = v;
    }
}

void parse_gsa(const std::vector<std::string> &f, NmeaMsg &m)
{
    int mode = 0;
    if (parse_int(field(f, 2), mode) && mode >= 1 && mode <= 3) m.mode = mode;
    m.gsa_manual = field(f, 1) == "M";
    int sys = 0;
    if (parse_int(field(f, 18), sys) && sys <= 9) m.system_id = sys;
    for (std::size_t i = 3; i <= 14; ++i) { /* the twelve satellite fields */
        int prn = 0;
        if (parse_int(field(f, i), prn) && prn >= 1 && prn <= 999) m.used_prns.push_back(prn);
    }
    double v = 0; /* PDOP 15, HDOP 16, VDOP 17 (fields 0 = address) */
    if (parse_double(field(f, 15), v) && v >= 0 && v < 100) {
        m.has_pdop = true;
        m.pdop = v;
    }
    if (parse_double(field(f, 16), v) && v >= 0 && v < 100) {
        m.has_hdop = true;
        m.hdop = v;
    }
    if (parse_double(field(f, 17), v) && v >= 0 && v < 100) {
        m.has_vdop = true;
        m.vdop = v;
    }
}

/* $--GSV,total,number,inview,{prn,elev,azim,snr}x1..4[,signal id]: the blocks start at field 4; a field count of
 * 4 + 4k + 1 means the NMEA 4.1 signal id follows the last block. */
void parse_gsv(const std::vector<std::string> &f, NmeaMsg &m)
{
    int n = 0;
    if (parse_int(field(f, 3), n) && n <= 99) {
        m.has_in_view = true;
        m.in_view = n;
    }
    int total = 0, num = 0;
    if (!parse_int(field(f, 1), total) || !parse_int(field(f, 2), num) || total < 1 || total > 9 || num < 1 || num > total) return;
    m.gsv_total = total;
    m.gsv_num = num;
    std::size_t blocks = f.size() > 4 ? (f.size() - 4) / 4 : 0;
    if (blocks > 4) blocks = 4;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t at = 4 + 4 * b;
        GsvSat s;
        int v = 0;
        if (!parse_int(field(f, at), v) || v < 1 || v > 999) continue; /* an empty block */
        s.prn = v;
        if (parse_int(field(f, at + 1), v) && v <= 90) s.elev = v;
        if (parse_int(field(f, at + 2), v) && v <= 360) s.azim = v == 360 ? 0 : v;
        if (parse_int(field(f, at + 3), v) && v <= 99) s.snr = v;
        m.gsv_sats.push_back(s);
    }
    if (f.size() > 4 && (f.size() - 4) % 4 == 1) {
        int sig = 0;
        if (parse_int(f.back(), sig) && sig <= 15) m.gsv_signal = sig;
    }
}

} // namespace

std::uint8_t nmea_checksum(const std::string &body)
{
    std::uint8_t x = 0;
    for (char c : body) x ^= static_cast<std::uint8_t>(c);
    return x;
}

std::string nmea_frame(const std::string &body)
{
    char tail[8];
    std::snprintf(tail, sizeof tail, "*%02X", nmea_checksum(body));
    return "$" + body + tail;
}

bool nmea_checksum_ok(const std::string &line)
{
    if (line.size() < 5 || line[0] != '$') return false;
    const std::size_t star = line.rfind('*');
    if (star == std::string::npos || star + 3 != line.size()) return false;
    unsigned want = 0;
    for (std::size_t i = star + 1; i < line.size(); ++i) {
        const char c = line[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else return false;
        want = want * 16 + d;
    }
    return nmea_checksum(line.substr(1, star - 1)) == want;
}

NmeaMsg parse_nmea(const std::string &line)
{
    NmeaMsg m;
    if (!nmea_checksum_ok(line)) return m;
    const std::string body = line.substr(1, line.rfind('*') - 1);
    const std::vector<std::string> f = split_fields(body);
    const std::string &addr = f[0];
    if (addr.size() < 3) return m;
    for (char c : addr)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return m;
    m.kind = MsgKind::Other;
    m.type = addr;
    if (addr.size() == 5 && addr[0] != 'P') {
        const std::string t = addr.substr(2);
        m.talker[0] = addr[0];
        m.talker[1] = addr[1];
        if (t == "RMC") m.kind = MsgKind::Rmc;
        else if (t == "GGA") m.kind = MsgKind::Gga;
        else if (t == "GSA") m.kind = MsgKind::Gsa;
        else if (t == "GSV") m.kind = MsgKind::Gsv;
        if (m.kind != MsgKind::Other) m.type = t;
    }
    switch (m.kind) {
    case MsgKind::Rmc: parse_rmc(f, m); break;
    case MsgKind::Gga: parse_gga(f, m); break;
    case MsgKind::Gsa: parse_gsa(f, m); break;
    case MsgKind::Gsv: parse_gsv(f, m); break;
    default: break;
    }
    return m;
}

void NmeaSplitter::reset()
{
    line_.clear();
    in_line_ = false;
}

void NmeaSplitter::feed(const std::uint8_t *data, std::size_t n, std::vector<std::string> &out)
{
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = data[i];
        if (b == '$') {
            line_.assign(1, '$');
            in_line_ = true;
            continue;
        }
        if (!in_line_) continue;
        if (b == '\r' || b == '\n') {
            if (line_.size() > 1) out.push_back(line_);
            line_.clear();
            in_line_ = false;
            continue;
        }
        if (b < 0x20 || b > 0x7E || line_.size() >= kMaxLine) { /* binary or too long: not a sentence */
            line_.clear();
            in_line_ = false;
            continue;
        }
        line_ += static_cast<char>(b);
    }
}

} // namespace atlas
