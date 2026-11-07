/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS status page, everything that is not drawing. The texts of its cards (receiver, position, DOP,
 * satellites) made from a GpsStatus (gps_fix.hpp), the geometry of the sky plot (azimuth / elevation to x / y, N up, the
 * 30 and 60 degree rings, which PRN labels have room), the layout of the signal bars (bar heights on the 0-50 dB-Hz
 * scale, how many cells fit in a row and where the rows wrap), the six constellation colours, how the page is opened and
 * left (chip tap, the S key, the Settings row; Esc goes back to where it came from) and what changed since the last
 * look (the page redraws only then). Pure: no LVGL, no clock besides the parameter (CLOCK_MONOTONIC in ms).
 */
#pragma once

#include "gps_chip.hpp"
#include "gps_fix.hpp"
#include "gps_sky.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

/* "unknown": a hyphen (the launcher's fonts have no em dash). */
extern const char *const kUnknownText;

/* ------------------------------------------------------------------------------------------------ texts */

/* "48.889643 N" (6 decimals, the hemisphere as a letter, the magnitude); kUnknownText when v is NaN. */
std::string lat_text(double lat);
std::string lon_text(double lon);
/* "41.5 m" (MSL from GGA); unknown without an altitude (2D fix). */
std::string altitude_text(const Fix &f);
/* "+-5 m": the HDOP-based radius the ring on the map uses (accuracy_from_hdop). */
std::string accuracy_text(const Fix &f);
/* "12 km/h (3.4 m/s)" or "7.6 mph (3.4 m/s)"; one decimal below 10, none from 10 up; unknown for a negative speed. */
std::string speed_text(double mps, bool miles);
/* "N", "NE", "E", ... (eight points, 45 degrees wide each, N centred on 0). */
const char *compass_word(double deg);
/* "70 deg NE" with the degree sign; unknown for a negative course. */
std::string course_text(double deg);
/* "2026-01-15" and "10:15:30" from the receiver's "2026-01-15T10:15:30Z"; unknown when empty or malformed. */
std::string utc_date_text(const std::string &iso);
std::string utc_time_text(const std::string &iso);
/* "0.9"; unknown for a negative value. */
std::string dop_text(double v);
/* "none" / "2D" / "3D" from a GSA mode (1, 2, 3); unknown for 0. */
const char *fix_mode_text(int gsa_mode);
/* "12 s ago", "3 min ago"; "no data yet" when last_ms is 0. */
std::string age_text(std::int64_t last_ms, std::int64_t now_ms);

/* The cards. Every field is the finished text; an equal GpsPageText needs no redraw. */
struct GpsPageText {
    bool off = false;                  /* GPS Off: only `headline` is shown */
    std::string headline;              /* "3D fix", "Searching for satellites", ... */
    std::string detail;                /* a second, smaller line (chip_description, a hint); may be empty */
    ChipState chip;                    /* the dish chip drawn next to the headline */
    /* receiver */
    std::string device, port, speed, rate, last_data, bad_checksums, chosen;
    /* position */
    std::string lat, lon, altitude, accuracy, speed_over_ground, course, utc_date, utc_time;
    /* DOP */
    std::string hdop, vdop, pdop, mode, used;
    /* satellites */
    std::string in_view, in_fix, per_system;
    bool operator==(const GpsPageText &o) const;
    bool operator!=(const GpsPageText &o) const { return !(*this == o); }
};

/* The texts at now_ms. miles: the Units setting (km/h or mph). */
GpsPageText gps_page_text(const GpsStatus &s, bool miles, std::int64_t now_ms);

/* The Settings row's value: "Off", "No receiver", "Searching", "2D fix", "3D fix", "Signal lost". */
std::string gps_row_value(const GpsStatus &s);

/* "G 9  R 4  S 1" counts per constellation in view, only the non-empty ones, in the table's order. */
std::string per_system_text(const std::vector<SkySat> &sats);

/* ------------------------------------------------------------------------------------------------ colours */

/* The six constellation colours (a colour-blind-safe set after Okabe and Ito: sky blue, reddish purple, yellow, violet,
 * light grey, teal; none is the chip's red, orange or green). Unknown: mid grey. */
std::uint32_t constellation_rgb(Constellation c);

/* ------------------------------------------------------------------------------------------------ sky plot */

struct SkyGeom {
    double cx = 0, cy = 0;   /* the centre in the plot's box */
    double r = 0;            /* the horizon's radius (elevation 0) */
};
struct SkyPoint {
    double x = 0, y = 0;
};
/* The circle in a w x h box with `margin` pixels kept free all round (the N E S W letters live there). */
SkyGeom sky_geom(int w, int h, int margin);
/* The radius of the ring at an elevation: 0 degrees is r, 90 is the centre. */
double sky_ring_radius(const SkyGeom &g, int elev_deg);
/* N up, E to the right; elevation 90 is the centre. */
SkyPoint sky_point(const SkyGeom &g, int elev_deg, int azim_deg);
/* A satellite that can be placed (elevation 0-90 and azimuth 0-360 known). */
bool sky_placeable(const SkySat &s);

/* Which PRN labels have room: a label is a w x h box to the right of its dot (dot radius `dot_r`); it is shown when it lies
 * inside the box 0..box_w x 0..box_h, touches no dot (any satellite, a dot is a square of side 2 x dot_r) and no label
 * already shown (the satellites in order). Satellites that cannot be placed get false. */
std::vector<bool> sky_labels_fit(const std::vector<SkySat> &sats, const SkyGeom &g, int dot_r, int label_w, int label_h,
                                 int box_w, int box_h);

/* ------------------------------------------------------------------------------------------------ signal bars */

constexpr int kSnrMax = 50;   /* the bars' scale: 0 to 50 dB-Hz */

/* The bar's height in pixels (0 for no SNR or 0 dB-Hz; kSnrMax and above fill the plot). */
int bar_height(int snr, int plot_h);

struct BarCell {
    int row = 0, col = 0;
    int x = 0, w = 0;            /* the cell (label column) in the plot's box, left to right */
    int bar_x = 0, bar_w = 0;    /* the bar inside it */
};
struct BarLayout {
    int rows = 0, per_row = 0;
    int axis_w = 0;              /* the scale's column at the left of every row */
    int cell_w = 0;
    std::vector<BarCell> cells;
};
/* n bars in a box avail_w wide: after the axis column, as many cells of at least label_w + 2 px as fit in a row; more
 * satellites than that wrap into rows of equal length (never a horizontal overflow); a cell is at most 3 x label_w wide. */
BarLayout bar_layout(int n, int avail_w, int axis_w, int label_w);

/* ------------------------------------------------------------------------------------------------ opening and leaving */

enum class StatusOrigin { None, Map, Settings };

/* How the page is opened and where Esc returns to. The chip and the S key exist on the map with GPS on; the Settings row
 * opens the page whatever the GPS setting says (the page then explains that it is off). */
class StatusNav {
public:
    /* True when the page opens (an already open page ignores a second request). */
    bool open_from(StatusOrigin from, bool gps_on);
    bool is_open() const { return open_; }
    StatusOrigin origin() const { return from_; }
    /* Leaves the page: the place to return to (None when it was not open). */
    StatusOrigin leave();

private:
    bool open_ = false;
    StatusOrigin from_ = StatusOrigin::None;
};

/* What changed since the last look: the page redraws only the parts that did. */
struct GpsPageChange {
    bool text = false;   /* the texts (cards, headline) */
    bool sky = false;    /* the satellites' places, used flags, constellations (sky plot) */
    bool bars = false;   /* the SNRs, used flags, order (signal bars) */
    bool any() const { return text || sky || bars; }
};
class GpsPageLive {
public:
    /* Compares with the previous call; the first call reports everything as changed. */
    GpsPageChange update(const GpsPageText &t, const std::vector<SkySat> &sats);
    void reset() { first_ = true; }

private:
    bool first_ = true;
    GpsPageText text_;
    std::vector<SkySat> sats_;
};

} // namespace atlas
