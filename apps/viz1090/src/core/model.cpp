/*
 * SPDX-License-Identifier: MIT
 *
 * viz1090 intro: texts and rules of the pages (see model.hpp).
 */
#include "model.hpp"

namespace viz {

const char *const kCheck = "\xEF\x80\x8C"; /* U+F00C */
const char *const kCross = "\xEF\x80\x8D"; /* U+F00D */

/* ---------------------------------------------------------------- navigation */

bool Nav::back()
{
    if (stack_.size() <= 1) return false;
    stack_.pop_back();
    return true;
}

std::vector<std::string> Nav::title_path() const
{
    std::vector<std::string> path{"viz1090"};
    for (std::size_t i = 1; i < stack_.size(); ++i) {
        switch (stack_[i]) {
        case View::Position: path.push_back("Position"); break;
        case View::Latitude: path.push_back("Latitude"); break;
        case View::Longitude: path.push_back("Longitude"); break;
        default: break;
        }
    }
    return path;
}

/* ---------------------------------------------------------------- the position */

bool Setup::position(Place &out) const
{
    Place p;
    if (!parse_degrees(lat, kLatLimit, p.lat) || !parse_degrees(lon, kLonLimit, p.lon)) return false;
    p.code = code;
    out = p;
    return true;
}

void Setup::set_place(const Place &place)
{
    lat = degrees_text(place.lat);
    lon = degrees_text(place.lon);
    code = place.code;
}

void Setup::set_lat(double value)
{
    lat = degrees_text(value);
    code.clear();
}

void Setup::set_lon(double value)
{
    lon = degrees_text(value);
    code.clear();
}

/* ---------------------------------------------------------------- rows */

RowText sdr_row(const std::string &sdr)
{
    RowText r;
    r.label = "SDR receiver";
    if (!sdr.empty()) {
        r.values = {std::string(kCheck) + " Found"};
        r.tone = Tone::Ok;
    } else {
        r.values = {std::string(kCross) + " Not found: plug it in", std::string(kCross) + " Not found"};
        r.tone = Tone::Bad;
    }
    return r;
}

RowText gps_row(const GpsSnapshot &gps)
{
    RowText r;
    r.label = "GPS receiver";
    switch (gps.state) {
    case GpsState::None:
        if (!gps.denied.empty()) { /* a port the deck user may not read: the status line says how to fix it */
            r.values = {std::string(kCross) + " No access to /dev/" + gps.denied, std::string(kCross) + " No access"};
            r.tone = Tone::Bad;
        } else {
            r.values = {"None detected"};
            r.tone = Tone::Normal;
        }
        break;
    case GpsState::Waiting: {
        const std::string dev = "/dev/" + gps.port;
        r.values = {"Found on " + dev + ", waiting for a fix", dev + ": waiting for a fix", "Waiting for a fix",
                    "No fix yet"};
        r.tone = Tone::Warn;
        break;
    }
    case GpsState::Fix: {
        const std::string fix = std::string(kCheck) + " Fix";
        r.values = {fix + ": " + coords_text(gps.lat, gps.lon), fix + ": " + coords_text(gps.lat, gps.lon, 3),
                    fix + ": " + coords_text(gps.lat, gps.lon, 2), fix};
        r.tone = Tone::Ok;
        break;
    }
    }
    return r;
}

RowText position_row(const Setup &setup)
{
    RowText r;
    r.label = "Position";
    Place p;
    if (setup.position(p)) {
        r.values = {place_text(p)};
        if (!p.code.empty()) r.values.push_back(p.code);
        else r.values.push_back(coords_text(p.lat, p.lon, 2));
        r.tone = Tone::Normal;
    } else {
        r.values = {"Not set"};
        r.tone = Tone::Warn;
    }
    return r;
}

namespace {

RowText degrees_row(const char *label, const std::string &text, double limit)
{
    RowText r;
    r.label = label;
    double v = 0;
    if (parse_degrees(text, limit, v)) {
        r.values = {short_degrees(v)};
        r.tone = Tone::Normal;
    } else {
        r.values = {"Not set"};
        r.tone = Tone::Warn;
    }
    return r;
}

} // namespace

RowText latitude_row(const Setup &setup)
{
    return degrees_row("Latitude", setup.lat, kLatLimit);
}

RowText longitude_row(const Setup &setup)
{
    return degrees_row("Longitude", setup.lon, kLonLimit);
}

RowText recent_row(const Place &place)
{
    RowText r;
    if (place.code.empty()) {
        r.label = coords_text(place.lat, place.lon);
        r.values = {""};
    } else {
        r.label = place.code;
        r.values = {coords_text(place.lat, place.lon)};
    }
    r.tone = Tone::Normal;
    return r;
}

/* ---------------------------------------------------------------- the status line */

Band root_band(bool installed, const std::string &sdr, const Setup &setup)
{
    Place p;
    if (!installed) return {"viz1090 is not installed on this deck", Tone::Bad};
    if (!setup.position(p)) return {"Choose your position under Position, then Start", Tone::Normal};
    if (sdr.empty()) return {"No SDR receiver: plug the dongle into a USB port", Tone::Warn};
    return {"Start opens the map around " + (p.code.empty() ? coords_text(p.lat, p.lon) : p.code), Tone::Normal};
}

StartCheck check_start(bool installed, const Setup &setup)
{
    Place p;
    if (!installed) return StartCheck::NotInstalled;
    if (!setup.position(p)) return StartCheck::NoPosition;
    return StartCheck::Ok;
}

Band refusal_band(StartCheck check)
{
    switch (check) {
    case StartCheck::NotInstalled: return {"viz1090 is not installed on this deck", Tone::Bad};
    case StartCheck::NoPosition: return {"No position yet: choose one under Position first", Tone::Warn};
    default: return {"", Tone::Normal};
    }
}

Band sdr_band(const std::string &sdr)
{
    if (sdr.empty()) return {"No SDR receiver: plug the dongle into a USB port", Tone::Warn};
    return {"SDR receiver: " + sdr, Tone::Ok};
}

Band gps_band(const GpsSnapshot &gps)
{
    switch (gps.state) {
    case GpsState::Fix: return {"Position set from the GPS: " + coords_text(gps.lat, gps.lon), Tone::Ok};
    case GpsState::Waiting: /* the row may be too narrow for the port: the line says it */
        return {"GPS on /dev/" + gps.port + ": no fix yet, it needs open sky", Tone::Warn};
    default:
        if (!gps.denied.empty())
            return {"No access to /dev/" + gps.denied + ": join the dialout group", Tone::Bad};
        return {"No GPS receiver: plug in a USB GPS (NMEA)", Tone::Normal};
    }
}

Band place_set_band(const Place &place)
{
    return {"Position set: " + place_text(place), Tone::Ok};
}

Band code_hint(const std::string &typed, const AirportList &airports)
{
    if (!airports.loaded()) return {"No airport list: use Latitude and Longitude", Tone::Warn};
    if (typed.size() < 3) return {"IATA code of the nearest airport, e.g. CDG", Tone::Normal};
    if (airports.find(typed)) return {"found: " + typed, Tone::Ok};
    return {"not in the list", Tone::Bad};
}

Band degrees_hint(bool latitude)
{
    return latitude ? Band{"From -90 to 90, south is negative", Tone::Normal}
                    : Band{"From -180 to 180, west is negative", Tone::Normal};
}

} // namespace viz
