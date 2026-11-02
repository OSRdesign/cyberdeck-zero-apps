/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the dashboard's tiles without LVGL (tests/test_tiles.cpp): the tabs (Favourites, one per
 * area, All), each tile's colours, glyph rule and state word, the grid's columns from the width in millimetres, the
 * rows of tiles and slider rows, the 2-D focus and the keys.
 *
 * Colours (study 053 section 6.4, dark only): Home Assistant's dark surface tokens and its state colours, which are
 * Material palette values (facts, not code): the tile #1c1c1c, the idle icon circle #282828, disabled #6f6f6f, inactive
 * #9e9e9e; "on" is amber INSIDE tiles only and always comes with the state word; the launcher's green / amber / red keep
 * their status meaning everywhere else. An active tile's circle is its state colour at 20 % over the tile.
 */
#pragma once

#include "model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ha {

namespace rgb {
constexpr std::uint32_t kTile = 0x1C1C1C;       /* --card-background-color */
constexpr std::uint32_t kCircle = 0x282828;     /* --secondary-background-color: the idle icon circle */
constexpr std::uint32_t kText = 0xE1E1E1;       /* --primary-text-color */
constexpr std::uint32_t kSecondary = 0x9B9B9B;  /* --secondary-text-color: the state word */
constexpr std::uint32_t kDisabled = 0x6F6F6F;   /* --disabled-text-color */
constexpr std::uint32_t kInactive = 0x9E9E9E;   /* an entity that is off */
constexpr std::uint32_t kAmber = 0xFFC107;      /* on, active */
constexpr std::uint32_t kPurple = 0x926BC7;     /* cover open */
constexpr std::uint32_t kCyan = 0x00BCD4;       /* fan */
constexpr std::uint32_t kDeepOrange = 0xFF6F22; /* heating */
constexpr std::uint32_t kBlue = 0x2196F3;       /* cooling */
constexpr std::uint32_t kGreen = 0x4CAF50;      /* auto, locked, battery high */
constexpr std::uint32_t kOrange = 0xFF9800;     /* dry, locking, battery medium */
constexpr std::uint32_t kRed = 0xF44336;        /* unlocked, jammed, alarm, battery low */
constexpr int kActivePct = 20;                  /* the circle: the state colour at 20 % over the tile */
} // namespace rgb

/* fg over bg, fg at pct percent (0..100). */
std::uint32_t mix_rgb(std::uint32_t fg, std::uint32_t bg, int pct);

struct TileLook {
    bool active = false;      /* the circle tinted with the state colour */
    bool unavailable = false; /* the tile dimmed, "Unavailable", the glyph disabled grey */
    std::uint32_t glyph = rgb::kInactive;
    std::uint32_t circle = rgb::kCircle;
};
/* The look of a tile for its domain and state (no state yet: idle). */
TileLook tile_look(Domain d, const State *s);

/* A unit for a tile, whose text face (FreeType Montserrat) has the degree sign: "°C" kept, other non-ASCII dropped. */
std::string tile_unit(const std::string &unit);
/* The tile's second line: "On, 72 %", "Off", "Locked", "21.5 °C", "Open, 60 %", "Heat, 21.4 °C", "Scene",
 * "Unavailable". temp_unit as Home Assistant gives it ("°C"). Never empty. */
std::string tile_word(Domain d, const State &s, int precision, const std::string &temp_unit);

/* What a tap on the icon (or Enter) does. */
enum class TileAction {
    Toggle,  /* light, switch, input_boolean */
    Run,     /* scene, script */
    Lock,    /* lock / unlock, asks first */
    Cover,   /* open / close; garage, gate and door covers ask first */
    Details  /* sensor, binary_sensor, climate: the entity page */
};
TileAction tile_action(Domain d);
/* The tile's page offers more than its icon's action, and so the tile shows the chevron: a dimmable light (the
 * brightness), a cover (both directions, its position), an entity read on its page (sensor, binary_sensor, climate,
 * whose icon opens the page too). A plain toggle, a scene, a script or a lock only repeat the action there: no
 * chevron, though a tap on the name still opens the page. */
bool tile_has_more(Domain d, TileAction a, bool dimmable);

/* Tile columns for a content width (study 053 section 6.2): clamp(floor((w_mm - 2 margin + gap) / (24 mm + gap)), 2, 6),
 * every length from px through the density (ppmm_x100: px per mm x 100). */
int grid_columns(int width_px, int ppmm_x100, int margin_px, int gap_px);
constexpr int kTileMinMmX10 = 240;

/* The views: Favourites, one per area holding visible entities (the areas by name, as All's ROOMS), then All. */
struct Tab {
    enum class Kind { Favourites, Area, All };
    Kind kind = Kind::Favourites;
    std::string area; /* Area: its id */
    std::string title;
    std::string key() const; /* "fav", "area:<id>", "all": what the app remembers across rebuilds */
};
std::vector<Tab> make_tabs(const std::vector<RoomInfo> &rooms);
/* The index of key in tabs, else 0 (Favourites). */
int tab_index(const std::vector<Tab> &tabs, const std::string &key);

/* The grid: rows of up to `columns` tiles, each tile row followed by one full-width slider row per dimmable light in it
 * (in column order). */
struct GridRow {
    bool slider = false;
    std::vector<int> tiles; /* tile indices; a slider row holds its light */
};
std::vector<GridRow> grid_rows(const std::vector<bool> &has_slider, int columns);

/* Keyboard focus in the grid: row, and the column in a tile row (0 in a slider row). */
struct Focus {
    int row = 0;
    int col = 0;
    int want_col = 0; /* the column Up / Down aim for (kept across slider rows) */
};
enum class NavKey { None, Up, Down, Left, Right, Home, End, Enter, Details, PrevView, NextView, Back };
/* evdev code -> what it does on the dashboard (Ctrl+O = details, Page Up / Down = views). */
NavKey nav_key(std::uint32_t evdev_code, bool ctrl);

enum class Move {
    Moved,  /* the focus changed */
    Stayed, /* at an edge: nothing changes */
    Top,    /* Up from the first row: the tab bar takes the focus */
    Slider  /* Left / Right on a slider row: the slider's 10 % steps */
};
Move move_focus(const std::vector<GridRow> &rows, Focus &f, NavKey k);
/* The tile index under the focus (-1 when rows is empty or the focus is out of range). */
int focused_tile(const std::vector<GridRow> &rows, const Focus &f);
/* Clamps a focus kept from an earlier grid into this one. */
void clamp_focus(const std::vector<GridRow> &rows, Focus &f);

} // namespace ha
