/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS status page's texts, geometry and state (see gps_page.hpp).
 */
#include "gps_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace atlas {

const char *const kUnknownText = "-";

namespace {

std::string fmt(const char *format, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

std::string coord_text(double v, bool is_lat)
{
    if (std::isnan(v)) return kUnknownText;
    char buf[48];
    const char hemi = is_lat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E');
    std::snprintf(buf, sizeof buf, "%.6f %c", std::fabs(v), hemi);
    return buf;
}

bool dop_known(double v) { return v >= 0 && v < 99.0; } /* 99.99 is what a receiver without a fix sends */

} // namespace

std::string lat_text(double lat) { return coord_text(lat, true); }
std::string lon_text(double lon) { return coord_text(lon, false); }

std::string altitude_text(const Fix &f)
{
    if (!f.has_alt) return kUnknownText;
    return fmt("%.1f m", f.alt_m);
}

std::string accuracy_text(const Fix &f)
{
    return "+-" + std::to_string(std::lround(accuracy_from_hdop(f.hdop))) + " m";
}

std::string speed_text(double mps, bool miles)
{
    if (mps < 0) return kUnknownText;
    const double v = mps * (miles ? 2.236936 : 3.6);
    const std::string main = fmt(v < 9.95 ? "%.1f" : "%.0f", v) + (miles ? " mph" : " km/h");
    return main + " (" + fmt("%.1f", mps) + " m/s)";
}

const char *compass_word(double deg)
{
    static const char *const kWords[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    double d = std::fmod(deg, 360.0);
    if (d < 0) d += 360.0;
    return kWords[static_cast<int>(std::floor((d + 22.5) / 45.0)) % 8];
}

std::string course_text(double deg)
{
    if (deg < 0) return kUnknownText;
    const int whole = static_cast<int>(std::lround(deg)) % 360;
    return std::to_string(whole) + "\xC2\xB0 " + compass_word(deg);
}

std::string utc_date_text(const std::string &iso)
{
    if (iso.size() < 19 || iso[10] != 'T') return kUnknownText;
    return iso.substr(0, 10);
}

std::string utc_time_text(const std::string &iso)
{
    if (iso.size() < 19 || iso[10] != 'T') return kUnknownText;
    return iso.substr(11, 8);
}

std::string dop_text(double v) { return dop_known(v) ? fmt("%.1f", v) : std::string(kUnknownText); }

const char *fix_mode_text(int gsa_mode)
{
    switch (gsa_mode) {
    case 1: return "none";
    case 2: return "2D";
    case 3: return "3D";
    default: return kUnknownText;
    }
}

std::string age_text(std::int64_t last_ms, std::int64_t now_ms)
{
    if (last_ms <= 0) return "no data yet";
    const long s = static_cast<long>(std::max<std::int64_t>(0, now_ms - last_ms) / 1000);
    if (s < 60) return std::to_string(s) + " s ago";
    return std::to_string(s / 60) + " min ago";
}

std::string per_system_text(const std::vector<SkySat> &sats)
{
    static const Constellation kOrder[] = {Constellation::Gps,     Constellation::Glonass, Constellation::Galileo,
                                           Constellation::Beidou,  Constellation::Qzss,    Constellation::Sbas,
                                           Constellation::Unknown};
    std::string out;
    for (Constellation c : kOrder) {
        int n = 0;
        for (const SkySat &s : sats) n += s.sys == c;
        if (!n) continue;
        if (!out.empty()) out += "  ";
        out += constellation_letter(c);
        out += " " + std::to_string(n);
    }
    return out;
}

bool GpsPageText::operator==(const GpsPageText &o) const
{
    return off == o.off && headline == o.headline && detail == o.detail && chip == o.chip && device == o.device &&
           port == o.port && speed == o.speed && rate == o.rate && last_data == o.last_data &&
           bad_checksums == o.bad_checksums && chosen == o.chosen && lat == o.lat && lon == o.lon &&
           altitude == o.altitude && accuracy == o.accuracy && speed_over_ground == o.speed_over_ground &&
           course == o.course && utc_date == o.utc_date && utc_time == o.utc_time && hdop == o.hdop && vdop == o.vdop && pdop == o.pdop && mode == o.mode &&
           used == o.used && in_view == o.in_view && in_fix == o.in_fix && per_system == o.per_system;
}

GpsPageText gps_page_text(const GpsStatus &s, bool miles, std::int64_t now_ms)
{
    GpsPageText t;
    const std::string dash = kUnknownText;
    t.chip = chip_state(s);
    if (s.kind == GpsStatus::Kind::Off) {
        t.off = true;
        t.headline = "GPS is off. Switch it on in Settings > GPS receiver.";
        return t;
    }
    const bool port_open = !s.link.port.empty();
    switch (s.kind) {
    case GpsStatus::Kind::NoDevice:
        t.headline = "No GPS receiver found";
        t.detail = "Plug the receiver into a USB port.";
        break;
    case GpsStatus::Kind::NoReceiver:
        t.headline = "No GPS receiver found";
        t.detail = "Serial ports are there, but none is a known receiver. Pick its port in Settings > GPS receiver.";
        break;
    case GpsStatus::Kind::NoAccess:
        t.headline = "No access to the port (dialout group?)";
        t.detail = "Add your user to the dialout group, then log in again.";
        break;
    case GpsStatus::Kind::Searching:
        if (!s.receiver_alive && port_open) {
            t.headline = "Receiver connected, no data";
            t.detail = "A port is open, but no valid sentence arrives. Check the speed in Settings > GPS receiver.";
        } else if (!s.receiver_alive) {
            t.headline = "No GPS receiver found";
            t.detail = "The receiver stopped sending.";
        } else {
            t.headline = "Searching for satellites";
            t.detail = chip_description(t.chip);
        }
        break;
    case GpsStatus::Kind::Fix2D:
    case GpsStatus::Kind::Fix3D:
        t.headline = s.kind == GpsStatus::Kind::Fix3D ? "3D fix" : "2D fix";
        t.detail = chip_description(t.chip);
        if (s.stale) t.detail += ", last fix " + age_text(s.fix.fix_ms, s.fix.fix_ms + s.age_ms);
        break;
    case GpsStatus::Kind::Off: break;
    }

    /* the receiver */
    t.device = t.port = t.speed = t.rate = t.last_data = t.bad_checksums = t.chosen = dash;
    if (port_open) {
        const GpsLinkFacts &l = s.link;
        if (!l.device.empty()) t.device = l.device;
        else if (l.vid) {
            char id[24];
            std::snprintf(id, sizeof id, "USB %04x:%04x", l.vid, l.pid);
            t.device = id;
        } else {
            t.device = "serial port";
        }
        t.port = l.port.compare(0, 5, "/dev/") == 0 ? l.port.substr(5) : l.port;
        if (l.baud > 0) t.speed = std::to_string(l.baud) + " baud" + (l.baud_auto ? " (auto)" : "");
        t.rate = fmt("%.1f", l.sentences_per_s) + " per s";
        t.last_data = age_text(l.last_valid_ms, now_ms);
        t.bad_checksums = std::to_string(l.bad_checksums);
        if (l.choice == PortChoice::AutoKnown) t.chosen = "Auto (known receiver)";
        else if (l.choice == PortChoice::ByUser) t.chosen = "Chosen by hand";
    }

    /* the position (a stale fix keeps its last values) */
    t.lat = t.lon = t.altitude = t.accuracy = t.speed_over_ground = t.course = t.utc_date = t.utc_time = dash;
    if (s.has_fix()) {
        const Fix &f = s.fix;
        t.lat = lat_text(f.lat);
        t.lon = lon_text(f.lon);
        t.altitude = altitude_text(f);
        t.accuracy = f.hdop >= 0 ? accuracy_text(f) : dash;
        t.speed_over_ground = speed_text(f.speed_mps, miles);
        t.course = course_text(f.course_deg);
        t.utc_date = utc_date_text(f.utc);
        t.utc_time = utc_time_text(f.utc);
    }

    /* the DOPs and the satellites */
    t.hdop = dop_text(s.dop.hdop);
    t.vdop = dop_text(s.dop.vdop);
    t.pdop = dop_text(s.dop.pdop);
    t.mode = t.used = t.in_view = t.in_fix = dash;
    int used = 0;
    for (const SkySat &sat : s.sats) used += sat.used;
    if (s.has_fix()) {
        t.mode = s.kind == GpsStatus::Kind::Fix3D ? "3D" : "2D";
        if (s.fix.sats_used >= 0) t.used = std::to_string(s.fix.sats_used);
        else if (!s.sats.empty()) t.used = std::to_string(used);
    } else if (s.receiver_alive) {
        t.mode = "none";
        t.used = std::to_string(used);
    }
    if (s.receiver_alive || s.has_fix()) {
        t.in_view = std::to_string(s.sats.size());
        t.in_fix = std::to_string(used);
        t.per_system = per_system_text(s.sats);
    }
    return t;
}

std::string gps_row_value(const GpsStatus &s)
{
    switch (s.kind) {
    case GpsStatus::Kind::Off: return "Off";
    case GpsStatus::Kind::NoDevice:
    case GpsStatus::Kind::NoReceiver:
    case GpsStatus::Kind::NoAccess: return "No receiver";
    case GpsStatus::Kind::Searching: return s.receiver_alive ? "Searching" : "No data";
    case GpsStatus::Kind::Fix2D:
    case GpsStatus::Kind::Fix3D:
        if (s.stale) return "Signal lost";
        return s.kind == GpsStatus::Kind::Fix3D ? "3D fix" : "2D fix";
    }
    return "";
}

std::uint32_t constellation_rgb(Constellation c)
{
    switch (c) {
    case Constellation::Gps: return 0x56B4E9;
    case Constellation::Glonass: return 0xCC79A7;
    case Constellation::Galileo: return 0xF0E442;
    case Constellation::Beidou: return 0x8E7CFF;
    case Constellation::Qzss: return 0xD0D0D0;
    case Constellation::Sbas: return 0x2EC4B6;
    case Constellation::Unknown: break;
    }
    return 0x808080;
}

/* ------------------------------------------------------------------------------------------------ sky plot */

SkyGeom sky_geom(int w, int h, int margin)
{
    SkyGeom g;
    g.cx = w / 2.0;
    g.cy = h / 2.0;
    g.r = std::max(1.0, std::min(w, h) / 2.0 - margin);
    return g;
}

double sky_ring_radius(const SkyGeom &g, int elev_deg)
{
    const int e = std::clamp(elev_deg, 0, 90);
    return g.r * (90 - e) / 90.0;
}

SkyPoint sky_point(const SkyGeom &g, int elev_deg, int azim_deg)
{
    const double rr = sky_ring_radius(g, elev_deg);
    const double a = azim_deg * M_PI / 180.0;
    SkyPoint p;
    p.x = g.cx + rr * std::sin(a);
    p.y = g.cy - rr * std::cos(a);
    return p;
}

bool sky_placeable(const SkySat &s) { return s.elev >= 0 && s.elev <= 90 && s.azim >= 0 && s.azim <= 360; }

namespace {
struct Box {
    int x0, y0, x1, y1;
};
bool overlap(const Box &a, const Box &b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }
} // namespace

std::vector<bool> sky_labels_fit(const std::vector<SkySat> &sats, const SkyGeom &g, int dot_r, int label_w, int label_h,
                                 int box_w, int box_h)
{
    std::vector<Box> dots(sats.size(), Box{0, 0, -1, -1});
    std::vector<int> cx(sats.size()), cy(sats.size());
    for (std::size_t i = 0; i < sats.size(); ++i) {
        if (!sky_placeable(sats[i])) continue;
        const SkyPoint p = sky_point(g, sats[i].elev, sats[i].azim);
        cx[i] = static_cast<int>(std::lround(p.x));
        cy[i] = static_cast<int>(std::lround(p.y));
        dots[i] = Box{cx[i] - dot_r, cy[i] - dot_r, cx[i] + dot_r, cy[i] + dot_r};
    }
    std::vector<bool> fit(sats.size(), false);
    std::vector<Box> shown;
    for (std::size_t i = 0; i < sats.size(); ++i) {
        if (!sky_placeable(sats[i])) continue;
        const Box lab{cx[i] + dot_r + 1, cy[i] - label_h / 2, cx[i] + dot_r + label_w, cy[i] - label_h / 2 + label_h - 1};
        if (lab.x0 < 0 || lab.y0 < 0 || lab.x1 >= box_w || lab.y1 >= box_h) continue;
        bool ok = true;
        for (std::size_t j = 0; j < sats.size() && ok; ++j)
            if (sky_placeable(sats[j]) && overlap(lab, dots[j])) ok = false;
        for (const Box &b : shown)
            if (ok && overlap(lab, b)) ok = false;
        if (!ok) continue;
        fit[i] = true;
        shown.push_back(lab);
    }
    return fit;
}

/* ------------------------------------------------------------------------------------------------ signal bars */

int bar_height(int snr, int plot_h)
{
    if (snr <= 0 || plot_h <= 0) return 0;
    const int v = std::min(snr, kSnrMax);
    return std::max(1, (v * plot_h + kSnrMax / 2) / kSnrMax);
}

BarLayout bar_layout(int n, int avail_w, int axis_w, int label_w)
{
    BarLayout L;
    L.axis_w = axis_w;
    if (n <= 0) return L;
    const int min_cell = label_w + 2;
    const int usable = std::max(avail_w - axis_w, min_cell);
    const int max_per = std::max(1, usable / min_cell);
    L.rows = (n + max_per - 1) / max_per;
    L.per_row = (n + L.rows - 1) / L.rows;
    L.cell_w = std::min(usable / L.per_row, 3 * label_w);
    const int gap = std::max(2, L.cell_w / 5);
    for (int i = 0; i < n; ++i) {
        BarCell c;
        c.row = i / L.per_row;
        c.col = i % L.per_row;
        c.x = axis_w + c.col * L.cell_w;
        c.w = L.cell_w;
        c.bar_w = std::max(2, L.cell_w - gap);
        c.bar_x = c.x + (L.cell_w - c.bar_w) / 2;
        L.cells.push_back(c);
    }
    return L;
}

/* ------------------------------------------------------------------------------------------------ opening and live */

bool StatusNav::open_from(StatusOrigin from, bool gps_on)
{
    if (open_ || from == StatusOrigin::None) return false;
    if (from == StatusOrigin::Map && !gps_on) return false;
    open_ = true;
    from_ = from;
    return true;
}

StatusOrigin StatusNav::leave()
{
    if (!open_) return StatusOrigin::None;
    open_ = false;
    return from_;
}

GpsPageChange GpsPageLive::update(const GpsPageText &t, const std::vector<SkySat> &sats)
{
    GpsPageChange c;
    if (first_) {
        c.text = c.sky = c.bars = true;
    } else {
        c.text = t != text_;
        if (sats.size() != sats_.size()) {
            c.sky = c.bars = true;
        } else {
            for (std::size_t i = 0; i < sats.size(); ++i) {
                const SkySat &a = sats[i], &b = sats_[i];
                if (a.sys != b.sys || a.prn != b.prn || a.used != b.used) c.sky = c.bars = true;
                if (a.elev != b.elev || a.azim != b.azim) c.sky = true;
                if (a.snr != b.snr) c.bars = true;
            }
        }
    }
    first_ = false;
    text_ = t;
    sats_ = sats;
    return c;
}

} // namespace atlas
