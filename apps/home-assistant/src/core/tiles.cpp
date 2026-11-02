/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the dashboard's tiles (see tiles.hpp).
 */
#include "tiles.hpp"

#include "format.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <linux/input-event-codes.h>

namespace ha {

std::uint32_t mix_rgb(std::uint32_t fg, std::uint32_t bg, int pct)
{
    pct = std::clamp(pct, 0, 100);
    std::uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int f = static_cast<int>((fg >> shift) & 0xFF), b = static_cast<int>((bg >> shift) & 0xFF);
        const int c = (f * pct + b * (100 - pct) + 50) / 100;
        out |= static_cast<std::uint32_t>(c) << shift;
    }
    return out;
}

namespace {

bool alarm_class(const std::string &c)
{
    return c == "battery" || c == "carbon_monoxide" || c == "cold" || c == "gas" || c == "heat" || c == "moisture" ||
           c == "problem" || c == "safety" || c == "smoke" || c == "tamper";
}

TileLook lit(std::uint32_t colour)
{
    TileLook l;
    l.active = true;
    l.glyph = colour;
    l.circle = mix_rgb(colour, rgb::kTile, rgb::kActivePct);
    return l;
}

std::string capital(std::string s)
{
    for (char &c : s)
        if (c == '_') c = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

} // namespace

TileLook tile_look(Domain d, const State *s)
{
    TileLook idle;
    if (!s) return idle;
    const std::string &st = s->state;
    if (st == "unavailable") {
        TileLook l;
        l.unavailable = true;
        l.glyph = rgb::kDisabled;
        return l;
    }
    switch (d) {
    case Domain::Light:
    case Domain::Switch:
    case Domain::InputBoolean:
    case Domain::Script: return st == "on" ? lit(rgb::kAmber) : idle;
    case Domain::Cover: return st == "open" || st == "opening" || st == "closing" ? lit(rgb::kPurple) : idle;
    case Domain::Lock:
        if (st == "locked") return lit(rgb::kGreen);
        if (st == "unlocked" || st == "open" || st == "jammed") return lit(rgb::kRed);
        if (st == "locking" || st == "unlocking" || st == "opening") return lit(rgb::kOrange);
        return idle;
    case Domain::Climate:
        if (st == "heat") return lit(rgb::kDeepOrange);
        if (st == "cool") return lit(rgb::kBlue);
        if (st == "auto") return lit(rgb::kGreen);
        if (st == "dry") return lit(rgb::kOrange);
        if (st == "heat_cool") return lit(rgb::kAmber);
        if (st == "fan_only") return lit(rgb::kCyan);
        return idle;
    case Domain::BinarySensor:
        if (st != "on") return idle;
        return lit(alarm_class(s->device_class) ? rgb::kRed : rgb::kAmber);
    case Domain::Sensor: {
        if (s->device_class != "battery") return idle;
        char *end = nullptr;
        const double level = std::strtod(st.c_str(), &end);
        if (st.empty() || !end || *end != '\0' || !std::isfinite(level)) return idle;
        if (level <= 20) return lit(rgb::kRed);
        if (level <= 50) return lit(rgb::kOrange);
        return lit(rgb::kGreen);
    }
    default: return idle;
    }
}

std::string tile_unit(const std::string &unit)
{
    std::string out;
    for (std::size_t i = 0; i < unit.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(unit[i]);
        if (c < 0x80) out += static_cast<char>(c);
        else if (c == 0xC2 && i + 1 < unit.size() && static_cast<unsigned char>(unit[i + 1]) == 0xB0) {
            out += "\xC2\xB0";
            ++i;
        }
    }
    return trim(out);
}

static std::string tile_temperature(double t, const std::string &unit)
{
    if (std::isnan(t)) return "";
    char buf[32];
    if (std::fabs(t - std::round(t)) < 0.05) std::snprintf(buf, sizeof buf, "%.0f", t);
    else std::snprintf(buf, sizeof buf, "%.1f", t);
    const std::string u = tile_unit(unit);
    return u.empty() ? std::string(buf) : std::string(buf) + " " + u;
}

std::string tile_word(Domain d, const State &s, int precision, const std::string &temp_unit)
{
    if (s.state == "unavailable") return "Unavailable";
    if (d == Domain::Scene) return "Scene";
    if (d == Domain::Script) return s.state == "on" ? "Running" : "Script";
    if (s.state == "unknown" || s.state.empty()) return "Unknown";
    switch (d) {
    case Domain::Light:
        if (s.state == "on") { /* the brightness of a dimmable light only (an on / off light may report 255) */
            const int pct = brightness_pct(s);
            return s.dimmable && s.brightness >= 0 && pct > 0 ? "On, " + std::to_string(pct) + " %" : std::string("On");
        }
        return s.state == "off" ? "Off" : capital(s.state);
    case Domain::Switch:
    case Domain::InputBoolean: return s.state == "on" ? "On" : s.state == "off" ? "Off" : capital(s.state);
    case Domain::Sensor: {
        const std::string v = format_number(s.state, precision);
        const std::string u = tile_unit(s.unit);
        return u.empty() ? v : v + " " + u;
    }
    case Domain::BinarySensor: return state_label(d, s, precision, temp_unit);
    case Domain::Cover: {
        std::string w = capital(s.state);
        if (s.state == "open" && s.current_position > 0 && s.current_position < 100)
            w += ", " + std::to_string(s.current_position) + " %";
        return w;
    }
    case Domain::Lock: return capital(s.state);
    case Domain::Climate: {
        std::string w = s.state == "heat_cool" ? "Heat/cool" : s.state == "fan_only" ? "Fan" : capital(s.state);
        const std::string t = tile_temperature(s.current_temperature, temp_unit);
        return t.empty() ? w : w + ", " + t;
    }
    default: return capital(s.state);
    }
}

TileAction tile_action(Domain d)
{
    switch (d) {
    case Domain::Light:
    case Domain::Switch:
    case Domain::InputBoolean: return TileAction::Toggle;
    case Domain::Scene:
    case Domain::Script: return TileAction::Run;
    case Domain::Lock: return TileAction::Lock;
    case Domain::Cover: return TileAction::Cover;
    default: return TileAction::Details;
    }
}

bool tile_has_more(Domain d, TileAction a, bool dimmable)
{
    return a == TileAction::Details || d == Domain::Cover || (d == Domain::Light && dimmable);
}

int grid_columns(int width_px, int ppmm_x100, int margin_px, int gap_px)
{
    if (ppmm_x100 <= 0) ppmm_x100 = 1130;
    const double ppmm = ppmm_x100 / 100.0;
    const double w = width_px / ppmm, margin = margin_px / ppmm, gap = gap_px / ppmm;
    const int cols = static_cast<int>(std::floor((w - 2 * margin + gap) / (kTileMinMmX10 / 10.0 + gap) + 1e-9));
    return std::clamp(cols, 2, 6);
}

std::string Tab::key() const
{
    switch (kind) {
    case Kind::Favourites: return "fav";
    case Kind::Area: return "area:" + area;
    case Kind::All: return "all";
    case Kind::View: return "view:" + std::to_string(view);
    }
    return "fav";
}

std::vector<Tab> make_tabs(const std::vector<RoomInfo> &rooms, const std::vector<Tab> &views)
{
    std::vector<Tab> tabs(views.begin(), views.end());
    Tab fav;
    fav.kind = Tab::Kind::Favourites;
    fav.title = "Favourites";
    tabs.push_back(fav);
    for (const RoomInfo &r : rooms) {
        if (r.id.empty() || r.count <= 0) continue;
        Tab t;
        t.kind = Tab::Kind::Area;
        t.area = r.id;
        t.title = r.name;
        tabs.push_back(std::move(t));
    }
    Tab all;
    all.kind = Tab::Kind::All;
    all.title = "All";
    tabs.push_back(all);
    return tabs;
}

int tab_index(const std::vector<Tab> &tabs, const std::string &key)
{
    for (std::size_t i = 0; i < tabs.size(); ++i)
        if (tabs[i].key() == key) return static_cast<int>(i);
    return 0;
}

std::vector<GridRow> grid_rows(const std::vector<bool> &has_slider, int columns)
{
    std::vector<GridRow> rows;
    columns = std::max(1, columns);
    const int n = static_cast<int>(has_slider.size());
    for (int start = 0; start < n; start += columns) {
        GridRow r;
        for (int i = start; i < std::min(n, start + columns); ++i) r.tiles.push_back(i);
        rows.push_back(r);
        for (int i = start; i < std::min(n, start + columns); ++i)
            if (has_slider[static_cast<std::size_t>(i)]) {
                GridRow s;
                s.slider = true;
                s.tiles.push_back(i);
                rows.push_back(s);
            }
    }
    return rows;
}

NavKey nav_key(std::uint32_t code, bool ctrl)
{
    switch (code) {
    case KEY_UP: return NavKey::Up;
    case KEY_DOWN: return NavKey::Down;
    case KEY_LEFT: return NavKey::Left;
    case KEY_RIGHT: return NavKey::Right;
    case KEY_HOME: return NavKey::Home;
    case KEY_END: return NavKey::End;
    case KEY_ENTER:
    case KEY_KPENTER: return NavKey::Enter;
    case KEY_O: return ctrl ? NavKey::Details : NavKey::None;
    case KEY_PAGEUP: return NavKey::PrevView;
    case KEY_PAGEDOWN: return NavKey::NextView;
    case KEY_ESC: return NavKey::Back;
    default: return NavKey::None;
    }
}

int focused_tile(const std::vector<GridRow> &rows, const Focus &f)
{
    if (f.row < 0 || f.row >= static_cast<int>(rows.size())) return -1;
    const GridRow &r = rows[static_cast<std::size_t>(f.row)];
    if (r.tiles.empty()) return -1;
    const int col = r.slider ? 0 : std::clamp(f.col, 0, static_cast<int>(r.tiles.size()) - 1);
    return r.tiles[static_cast<std::size_t>(col)];
}

void clamp_focus(const std::vector<GridRow> &rows, Focus &f)
{
    if (rows.empty()) {
        f = Focus();
        return;
    }
    f.row = std::clamp(f.row, 0, static_cast<int>(rows.size()) - 1);
    const GridRow &r = rows[static_cast<std::size_t>(f.row)];
    f.col = r.slider ? 0 : std::clamp(f.col, 0, static_cast<int>(r.tiles.size()) - 1);
}

static void enter_row(const std::vector<GridRow> &rows, Focus &f, int row)
{
    f.row = row;
    const GridRow &r = rows[static_cast<std::size_t>(row)];
    f.col = r.slider ? 0 : std::min(f.want_col, static_cast<int>(r.tiles.size()) - 1);
}

Move move_focus(const std::vector<GridRow> &rows, Focus &f, NavKey k)
{
    if (rows.empty()) return k == NavKey::Up ? Move::Top : Move::Stayed;
    clamp_focus(rows, f);
    const int n = static_cast<int>(rows.size());
    const GridRow &cur = rows[static_cast<std::size_t>(f.row)];
    switch (k) {
    case NavKey::Left:
    case NavKey::Right: {
        if (cur.slider) return Move::Slider;
        const int to = f.col + (k == NavKey::Left ? -1 : 1);
        if (to < 0 || to >= static_cast<int>(cur.tiles.size())) return Move::Stayed;
        f.col = f.want_col = to;
        return Move::Moved;
    }
    case NavKey::Up: {
        if (cur.slider) { /* back to the light it belongs to */
            const int tile = cur.tiles[0];
            for (int r = f.row - 1; r >= 0; --r) {
                const GridRow &g = rows[static_cast<std::size_t>(r)];
                if (g.slider) continue;
                for (std::size_t c = 0; c < g.tiles.size(); ++c)
                    if (g.tiles[c] == tile) {
                        f.row = r;
                        f.col = f.want_col = static_cast<int>(c);
                        return Move::Moved;
                    }
            }
        }
        if (f.row == 0) return Move::Top;
        enter_row(rows, f, f.row - 1);
        return Move::Moved;
    }
    case NavKey::Down: {
        if (!cur.slider) { /* a dimmable light: into its own slider */
            const int tile = cur.tiles[static_cast<std::size_t>(f.col)];
            for (int r = f.row + 1; r < n && rows[static_cast<std::size_t>(r)].slider; ++r)
                if (rows[static_cast<std::size_t>(r)].tiles[0] == tile) {
                    f.row = r;
                    f.col = 0;
                    return Move::Moved;
                }
        }
        if (f.row + 1 >= n) return Move::Stayed;
        enter_row(rows, f, f.row + 1);
        return Move::Moved;
    }
    case NavKey::Home:
        if (f.row == 0 && f.col == 0) return Move::Stayed;
        f.row = f.col = f.want_col = 0;
        return Move::Moved;
    case NavKey::End: {
        const int last = n - 1;
        const int col = rows[static_cast<std::size_t>(last)].slider
                            ? 0
                            : static_cast<int>(rows[static_cast<std::size_t>(last)].tiles.size()) - 1;
        if (f.row == last && f.col == col) return Move::Stayed;
        f.row = last;
        f.col = f.want_col = col;
        return Move::Moved;
    }
    default: return Move::Stayed;
    }
}

} // namespace ha
