/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: masonry, toggles, forecast, history and words of the stored dashboard (see dash.hpp).
 */
#include "dash.hpp"

#include "format.hpp"
#include "tiles.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ha {

int masonry_columns(int width_px, int ppmm_x100)
{
    if (ppmm_x100 <= 0) ppmm_x100 = 1130;
    const double mm = width_px * 100.0 / ppmm_x100;
    return std::clamp(static_cast<int>(std::floor(mm / 50.0 + 1e-9)), 1, 4);
}

std::vector<int> masonry_place(const std::vector<int> &heights, int columns, int gap, std::vector<int> *ys)
{
    columns = std::max(1, columns);
    std::vector<int> col_h(static_cast<std::size_t>(columns), 0), out;
    if (ys) ys->clear();
    for (int h : heights) {
        int best = 0;
        for (int c = 1; c < columns; ++c)
            if (col_h[static_cast<std::size_t>(c)] < col_h[static_cast<std::size_t>(best)]) best = c;
        out.push_back(best);
        if (ys) ys->push_back(col_h[static_cast<std::size_t>(best)]);
        col_h[static_cast<std::size_t>(best)] += std::max(0, h) + gap;
    }
    return out;
}

bool toggle_domain(const std::string &id)
{
    const std::size_t dot = id.find('.');
    if (dot == std::string::npos) return false;
    const std::string d = id.substr(0, dot);
    return d == "light" || d == "switch" || d == "fan" || d == "input_boolean";
}

/* ------------------------------------------------------------------------------------------------- time */

namespace {

/* days since 1970-01-01 of a civil date (proleptic Gregorian) */
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
}

bool digits(const std::string &s, std::size_t at, std::size_t n, int &out)
{
    if (at + n > s.size()) return false;
    out = 0;
    for (std::size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}

std::string capital(std::string s)
{
    for (char &c : s)
        if (c == '_') c = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

bool numeric(const std::string &s, double &v)
{
    if (s.empty()) return false;
    char *end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}

} // namespace

std::string iso_utc(std::int64_t epoch)
{
    std::int64_t days = epoch / 86400, secs = epoch % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    int y = 1970;
    unsigned m = 1, d = 1;
    civil_from_days(days, y, m, d);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d+00:00", y, m, d, static_cast<int>(secs / 3600),
                  static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60));
    return buf;
}

bool parse_iso(const std::string &s, double &epoch)
{
    int y, mo, d, h, mi, se;
    if (!digits(s, 0, 4, y) || s.size() < 19 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' ||
        !digits(s, 8, 2, d) || (s[10] != 'T' && s[10] != ' ') || !digits(s, 11, 2, h) || s[13] != ':' ||
        !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, se))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
    std::size_t i = 19;
    double frac = 0;
    if (i < s.size() && s[i] == '.') {
        double scale = 0.1;
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10) frac += (s[i] - '0') * scale;
    }
    long offset = 0;
    if (i < s.size()) {
        if (s[i] == 'Z' && i + 1 == s.size()) {
        } else if ((s[i] == '+' || s[i] == '-') && s.size() == i + 6 && s[i + 3] == ':') {
            int oh, om;
            if (!digits(s, i + 1, 2, oh) || !digits(s, i + 4, 2, om)) return false;
            offset = (oh * 3600L + om * 60L) * (s[i] == '-' ? -1 : 1);
        } else {
            return false;
        }
    }
    epoch = static_cast<double>(days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
                                h * 3600 + mi * 60 + se - offset) +
            frac;
    return true;
}

/* ------------------------------------------------------------------------------------------------- weather */

namespace {

struct Condition {
    const char *state, *icon, *word;
};
/* Home Assistant's documented weather conditions and the Material Design Icons named for them */
const Condition kConditions[] = {
    {"clear-night", "mdi:weather-night", "Clear, night"},
    {"cloudy", "mdi:weather-cloudy", "Cloudy"},
    {"exceptional", "mdi:alert-circle-outline", "Exceptional"},
    {"fog", "mdi:weather-fog", "Fog"},
    {"hail", "mdi:weather-hail", "Hail"},
    {"lightning", "mdi:weather-lightning", "Lightning"},
    {"lightning-rainy", "mdi:weather-lightning-rainy", "Lightning, rainy"},
    {"partlycloudy", "mdi:weather-partly-cloudy", "Partly cloudy"},
    {"pouring", "mdi:weather-pouring", "Pouring"},
    {"rainy", "mdi:weather-rainy", "Rainy"},
    {"snowy", "mdi:weather-snowy", "Snowy"},
    {"snowy-rainy", "mdi:weather-snowy-rainy", "Snowy, rainy"},
    {"sunny", "mdi:weather-sunny", "Sunny"},
    {"windy", "mdi:weather-windy", "Windy"},
    {"windy-variant", "mdi:weather-windy-variant", "Windy"},
};

const Condition *condition(const std::string &state)
{
    for (const Condition &c : kConditions)
        if (state == c.state) return &c;
    return nullptr;
}

} // namespace

std::string weather_icon(const std::string &state)
{
    const Condition *c = condition(state);
    return c ? c->icon : std::string();
}

std::string weather_word(const std::string &state)
{
    if (const Condition *c = condition(state)) return c->word;
    if (state == "unavailable") return "Unavailable";
    if (state.empty() || state == "unknown") return "Unknown";
    std::string w = capital(state);
    for (char &ch : w)
        if (ch == '-') ch = ' ';
    return w;
}

bool parse_forecast(const json::Value &event, long utc_offset_s, std::vector<ForecastDay> &out, std::size_t max)
{
    out.clear();
    const json::Value *list = event.is_object() ? event.get("forecast") : nullptr;
    if (!list || !list->is_array()) return false;
    static const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (const json::Value &f : list->items) {
        if (out.size() >= max) break;
        if (!f.is_object()) continue;
        ForecastDay d;
        double t = 0;
        if (const json::Value *dt = f.get("datetime"))
            if (parse_iso(dt->as_string(), t)) {
                const std::int64_t local = static_cast<std::int64_t>(std::floor(t)) + utc_offset_s;
                std::int64_t days = local / 86400;
                if (local % 86400 < 0) --days;
                d.day = kDays[((days % 7) + 7 + 4) % 7];
            }
        if (const json::Value *c = f.get("condition")) d.condition = c->as_string();
        if (const json::Value *v = f.get("temperature"))
            if (v->is_number()) d.high = v->as_double(NAN);
        if (const json::Value *v = f.get("templow"))
            if (v->is_number()) d.low = v->as_double(NAN);
        out.push_back(std::move(d));
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------- history */

std::vector<HistoryPoint> parse_history(const json::Value &result, const std::string &entity)
{
    std::vector<HistoryPoint> out;
    const json::Value *list = result.is_object() ? result.get(entity) : nullptr;
    if (!list || !list->is_array()) return out;
    for (const json::Value &p : list->items) {
        if (!p.is_object()) continue;
        const json::Value *s = p.get("s");
        const json::Value *t = p.get("lu");
        if (!t) t = p.get("lc");
        double v = 0;
        if (!s || !t || !t->is_number() || !numeric(s->as_string(), v)) continue;
        out.push_back(HistoryPoint{t->as_double(0), v});
        if (out.size() >= 20000) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const HistoryPoint &a, const HistoryPoint &b) { return a.t < b.t; });
    return out;
}

std::vector<double> downsample(const std::vector<HistoryPoint> &pts, double start, double end, int buckets)
{
    if (buckets <= 0 || !(end > start)) return {};
    std::vector<double> out(static_cast<std::size_t>(buckets), NAN);
    const double w = (end - start) / buckets;
    std::size_t i = 0;
    double last = NAN;
    for (int b = 0; b < buckets; ++b) {
        const double b0 = start + b * w;
        const bool final = b == buckets - 1;
        const double b1 = b0 + w;
        while (i < pts.size() && pts[i].t < b0) last = pts[i++].v;
        double sum = 0;
        int n = 0;
        while (i < pts.size() && (final || pts[i].t < b1)) {
            sum += pts[i].v;
            last = pts[i++].v;
            ++n;
        }
        out[static_cast<std::size_t>(b)] = n ? sum / n : last;
    }
    return out;
}

int graph_points(int hours, int detail)
{
    return std::clamp(hours * (detail == 2 ? 6 : 1), 2, 100);
}

std::vector<int> scale_points(const std::vector<double> &values)
{
    double lo = INFINITY, hi = -INFINITY;
    for (double v : values)
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    std::vector<int> out;
    out.reserve(values.size());
    for (double v : values) {
        if (!std::isfinite(v)) out.push_back(-1);
        else if (hi - lo < 1e-9) out.push_back(500);
        else out.push_back(static_cast<int>(std::lround((v - lo) * 1000.0 / (hi - lo))));
    }
    return out;
}

/* ------------------------------------------------------------------------------------------------- words */

std::string card_temperature(double t, const std::string &unit)
{
    if (!std::isfinite(t)) return "";
    char buf[32];
    if (std::fabs(t - std::round(t)) < 0.05) std::snprintf(buf, sizeof buf, "%.0f", t);
    else std::snprintf(buf, sizeof buf, "%.1f", t);
    const std::string u = tile_unit(unit);
    return u.empty() ? std::string(buf) : std::string(buf) + " " + u;
}

std::string entity_word(const std::string &id, const State *s, int precision, const std::string &temp_unit)
{
    if (!s) return "";
    const Domain d = domain_of(id);
    if (supported(d)) return tile_word(d, *s, precision, temp_unit);
    if (s->state == "unavailable") return "Unavailable";
    if (s->state.empty() || s->state == "unknown") return "Unknown";
    const std::string dom = id.substr(0, id.find('.'));
    if (dom == "weather") return weather_word(s->state);
    if (dom == "person" || dom == "device_tracker") {
        if (s->state == "home") return "Home";
        if (s->state == "not_home") return "Away";
    }
    if (dom == "fan" || dom == "automation" || dom == "input_boolean") {
        if (s->state == "on") return "On";
        if (s->state == "off") return "Off";
    }
    double v = 0;
    if (numeric(s->state, v)) {
        const std::string n = format_number(s->state, precision);
        const std::string u = tile_unit(s->unit);
        return u.empty() ? n : n + " " + u;
    }
    return capital(s->state);
}

} // namespace ha
