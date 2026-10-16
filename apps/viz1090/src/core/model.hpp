/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: what the pages say and the rules behind them, without LVGL (unit-tested). The pages (ui/) only lay
 * these texts out.
 *
 *   Nav        Root > Position > Latitude | Longitude; back at the root = quit
 *   Setup      the position being chosen (texts as typed or read from the file, and the airport it came from)
 *   rows       the root's SDR receiver, GPS receiver and Position rows; the Position page's Latitude, Longitude and
 *              Recent rows. A status is a word and a shape (a check, a cross), never a colour alone. A row's value
 *              comes as candidates, longest first: the page shows the first one that fits beside the label.
 *   band       the status line of the root: a refusal, a note after an action, else a hint of what to do next
 *   start      Start is refused (with a reason) without a usable position or without viz1090 installed
 */
#pragma once

#include "airports.hpp"
#include "gps.hpp"
#include "place.hpp"

#include <string>
#include <vector>

namespace viz {

constexpr const char *kAppId = "viz1090";
constexpr const char *kAppTitle = "viz1090";
constexpr int kMinW = 640; /* = X-MinSize of root/usr/share/APPLaunch/applications/viz1090.desktop (tests check it) */
constexpr int kMinH = 480;

/* Muted: grey, for an absent thing or a plain setting (never the only signal of status) */
enum class Tone { Normal, Ok, Warn, Bad, Muted };

/* LVGL's LV_SYMBOL_OK and LV_SYMBOL_CLOSE (U+F00C, U+F00D: the check and the cross of the built-in fonts) */
extern const char *const kCheck;
extern const char *const kCross;

/* ---------------------------------------------------------------- navigation */

enum class View { Root, Position, Latitude, Longitude };

class Nav {
public:
    View current() const { return stack_.back(); }
    std::size_t depth() const { return stack_.size(); }
    void open(View view) { stack_.push_back(view); }
    /* one level up; false at the root (the app quits) */
    bool back();
    /* the header's title path: {"viz1090"}, {"viz1090", "Position"}, {"viz1090", "Position", "Latitude"} */
    std::vector<std::string> title_path() const;

private:
    std::vector<View> stack_{View::Root};
};

/* ---------------------------------------------------------------- the position being chosen */

struct Setup {
    std::string lat, lon; /* as read from the file or typed ("" = not set) */
    std::string code;     /* the airport the position came from ("" = none) */

    /* both texts are degrees in range */
    bool position(Place &out) const;
    void set_place(const Place &place);
    void set_lat(double value); /* a typed latitude: the airport goes */
    void set_lon(double value);
};

/* ---------------------------------------------------------------- rows */

struct RowText {
    std::string label;
    std::vector<std::string> values; /* longest first ("" = no value) */
    Tone tone = Tone::Normal;
};

/* sdr: the dongle's name, "" when none */
RowText sdr_row(const std::string &sdr);
RowText gps_row(const GpsSnapshot &gps);
RowText position_row(const Setup &setup);
RowText latitude_row(const Setup &setup);
RowText longitude_row(const Setup &setup);
/* a Recent row: the code as label and the coordinates as value, or the coordinates alone */
RowText recent_row(const Place &place);

/* ---------------------------------------------------------------- the status line */

struct Band {
    std::string text;
    Tone tone = Tone::Normal;
};

/* what the root says when nothing happened: not installed, then no position, then no dongle, then where Start goes */
Band root_band(bool installed, const std::string &sdr, const Setup &setup);

enum class StartCheck { Ok, NotInstalled, NoPosition };
StartCheck check_start(bool installed, const Setup &setup);
/* the refusal (Ok: empty) */
Band refusal_band(StartCheck check);

/* Enter or a tap on a row of the root */
Band sdr_band(const std::string &sdr);
Band gps_band(const GpsSnapshot &gps); /* a fix: "Position set from the GPS" */
Band place_set_band(const Place &place);

/* the Position page's line under the airport code field */
Band code_hint(const std::string &typed, const AirportList &airports);
/* the Latitude / Longitude page's line under its field */
Band degrees_hint(bool latitude);

} // namespace viz
