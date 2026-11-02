/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: the dashboard's core (0.2.0, study 053 P1): the tabs, the grid's columns by millimetres, the rows and
 * the 2-D focus, the keys, the colours / words / glyph rules of the tiles, the icon order, the MDI name table, the
 * layout cache, and the engine's icon requests.
 */
#include "test_util.hpp"

#include "engine.hpp"
#include "format.hpp"
#include "icons.hpp"
#include "json.hpp"
#include "mdi.hpp"
#include "sim.hpp"
#include "store.hpp"
#include "tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace ha;

namespace {

json::Value J(const std::string &text)
{
    json::Value v;
    if (!json::parse(text, v)) std::fprintf(stderr, "bad test JSON: %s\n", text.c_str());
    return v;
}

State st(const std::string &s, const std::string &cls = std::string())
{
    State x;
    x.state = s;
    x.device_class = cls;
    return x;
}

std::string mdi_path()
{
    return std::string(CDK_APP_DIR) + "/root/usr/share/APPLaunch/share/home-assistant/mdi-names.bin";
}

} // namespace

static void test_columns_and_tabs()
{
    /* clamp(floor((w_mm - 2 margin + gap) / (24 + gap)), 2, 6), margin = SPACE_M, gap = SPACE_S (deck 16 / 8 px) */
    CHECK(grid_columns(640, 1130, 16, 8) == 2);   /* the deck: 56.6 mm */
    CHECK(grid_columns(480, 610, 9, 5) == 3);     /* the Pi 3A+'s 3.5" panel: 78.7 mm */
    CHECK(grid_columns(480, 1130, 16, 8) == 2);   /* 480x320 at the harness's default density: 42.5 mm */
    CHECK(grid_columns(800, 920, 13, 7) == 3);    /* HyperPixel 87 mm */
    CHECK(grid_columns(800, 741, 10, 5) == 4);    /* the Pi 4B's 5" DSI: 108 mm */
    CHECK(grid_columns(800, 840, 12, 6) == 3);    /* the Pi 3A+'s 4.3" DSI: 95 mm */
    CHECK(grid_columns(1280, 860, 13, 7) == 5);   /* the tablet: 149 mm (the study's table guessed 6; the rule gives 5) */
    CHECK(grid_columns(1280, 860, 0, 0) == 6);    /* ... 6 without margins */
    CHECK(grid_columns(720, 1000, 14, 7) == 2);   /* HackBerry 72 mm */
    CHECK(grid_columns(480, 850, 12, 6) == 2);    /* portrait 480x800 */
    CHECK(grid_columns(800, 860, 13, 7) == 3);    /* tablet portrait 800x1280 */
    CHECK(grid_columns(4000, 860, 13, 7) == 6);   /* never more than 6 */
    CHECK(grid_columns(200, 1130, 16, 8) == 2);   /* never fewer than 2 */
    CHECK(grid_columns(640, 0, 16, 8) == 2);      /* no density: the deck's */

    std::vector<RoomInfo> rooms = {{"bed", "Bedroom", 3}, {"attic", "Attic", 0}, {"kitchen", "Kitchen", 2},
                                   {"", "No room", 4}};
    const auto tabs = make_tabs(rooms);
    CHECK(tabs.size() == 4); /* Favourites, Bedroom, Kitchen, All: the empty area and "No room" have no tab */
    CHECK_EQ(tabs[0].title, std::string("Favourites"));
    CHECK(tabs[0].kind == Tab::Kind::Favourites);
    CHECK_EQ(tabs[1].title, std::string("Bedroom"));
    CHECK_EQ(tabs[1].key(), std::string("area:bed"));
    CHECK_EQ(tabs[2].area, std::string("kitchen"));
    CHECK(tabs[3].kind == Tab::Kind::All);
    CHECK_EQ(tabs[3].key(), std::string("all"));
    CHECK(tab_index(tabs, "area:kitchen") == 2);
    CHECK(tab_index(tabs, "area:gone") == 0); /* an area deleted meanwhile: back to Favourites */
    CHECK(make_tabs({}).size() == 2);
}

static void test_grid_and_focus()
{
    /* five tiles, two columns, the first and the third dimmable */
    const auto rows = grid_rows({true, false, true, false, false}, 2);
    CHECK(rows.size() == 5);
    CHECK(!rows[0].slider && rows[0].tiles == std::vector<int>({0, 1}));
    CHECK(rows[1].slider && rows[1].tiles == std::vector<int>({0}));
    CHECK(rows[2].tiles == std::vector<int>({2, 3}));
    CHECK(rows[3].slider && rows[3].tiles[0] == 2);
    CHECK(rows[4].tiles == std::vector<int>({4}));
    /* two sliders under one row, in column order */
    const auto two = grid_rows({true, true, false}, 3);
    CHECK(two.size() == 3 && two[1].tiles[0] == 0 && two[2].tiles[0] == 1);
    CHECK(grid_rows({}, 2).empty());

    Focus f;
    CHECK(move_focus(rows, f, NavKey::Left) == Move::Stayed);
    CHECK(move_focus(rows, f, NavKey::Up) == Move::Top); /* the tab bar */
    CHECK(move_focus(rows, f, NavKey::Right) == Move::Moved);
    CHECK(focused_tile(rows, f) == 1);
    CHECK(move_focus(rows, f, NavKey::Right) == Move::Stayed);
    /* Down from tile 1 (no slider): the next row, which is tile 0's slider */
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Moved);
    CHECK(f.row == 1 && focused_tile(rows, f) == 0);
    CHECK(move_focus(rows, f, NavKey::Left) == Move::Slider); /* Left / Right step the slider */
    CHECK(move_focus(rows, f, NavKey::Right) == Move::Slider);
    /* Up from a slider: back to its light */
    CHECK(move_focus(rows, f, NavKey::Up) == Move::Moved);
    CHECK(f.row == 0 && f.col == 0);
    /* Down from a dimmable light: into its own slider */
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Moved);
    CHECK(f.row == 1);
    /* Down from a slider: the next tile row, in the column wanted (0) */
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Moved);
    CHECK(f.row == 2 && focused_tile(rows, f) == 2);
    CHECK(move_focus(rows, f, NavKey::Right) == Move::Moved);
    CHECK(focused_tile(rows, f) == 3);
    /* tile 3 has no slider: Down goes to row 3 (tile 2's slider), then to the last row's only tile */
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Moved);
    CHECK(f.row == 3);
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Moved);
    CHECK(f.row == 4 && focused_tile(rows, f) == 4); /* the wanted column 1 clamped to the row's one tile */
    CHECK(move_focus(rows, f, NavKey::Down) == Move::Stayed);
    CHECK(move_focus(rows, f, NavKey::Home) == Move::Moved);
    CHECK(f.row == 0 && f.col == 0);
    CHECK(move_focus(rows, f, NavKey::End) == Move::Moved);
    CHECK(focused_tile(rows, f) == 4);
    /* a focus kept from a longer grid is clamped */
    Focus far;
    far.row = 40;
    far.col = 9;
    clamp_focus(rows, far);
    CHECK(far.row == 4 && far.col == 0);
    Focus none;
    CHECK(move_focus({}, none, NavKey::Up) == Move::Top);
    CHECK(focused_tile({}, none) == -1);

    CHECK(nav_key(KEY_O, true) == NavKey::Details);
    CHECK(nav_key(KEY_O, false) == NavKey::None);
    CHECK(nav_key(KEY_ENTER, false) == NavKey::Enter);
    CHECK(nav_key(KEY_KPENTER, false) == NavKey::Enter);
    CHECK(nav_key(KEY_PAGEUP, false) == NavKey::PrevView);
    CHECK(nav_key(KEY_PAGEDOWN, false) == NavKey::NextView);
    CHECK(nav_key(KEY_ESC, false) == NavKey::Back);
    CHECK(nav_key(KEY_LEFT, false) == NavKey::Left);
    CHECK(nav_key(KEY_A, false) == NavKey::None);
}

static void test_looks_and_words()
{
    CHECK(mix_rgb(0xFFC107, rgb::kTile, 20) == 0x493D18u);
    CHECK(mix_rgb(0x123456, 0xABCDEF, 100) == 0x123456u);
    CHECK(mix_rgb(0x123456, 0xABCDEF, 0) == 0xABCDEFu);

    State on = st("on");
    TileLook l = tile_look(Domain::Light, &on);
    CHECK(l.active && l.glyph == rgb::kAmber && l.circle == mix_rgb(rgb::kAmber, rgb::kTile, 20));
    State off = st("off");
    l = tile_look(Domain::Light, &off);
    CHECK(!l.active && l.glyph == rgb::kInactive && l.circle == rgb::kCircle);
    State gone = st("unavailable");
    l = tile_look(Domain::Switch, &gone);
    CHECK(l.unavailable && !l.active && l.glyph == rgb::kDisabled);
    CHECK(!tile_look(Domain::Light, nullptr).active);
    State locked = st("locked"), unlocked = st("unlocked"), jammed = st("jammed"), locking = st("locking");
    CHECK(tile_look(Domain::Lock, &locked).glyph == rgb::kGreen);
    CHECK(tile_look(Domain::Lock, &unlocked).glyph == rgb::kRed);
    CHECK(tile_look(Domain::Lock, &jammed).glyph == rgb::kRed);
    CHECK(tile_look(Domain::Lock, &locking).glyph == rgb::kOrange);
    State heat = st("heat"), cool = st("cool"), coff = st("off"), autom = st("auto");
    CHECK(tile_look(Domain::Climate, &heat).glyph == rgb::kDeepOrange);
    CHECK(tile_look(Domain::Climate, &cool).glyph == rgb::kBlue);
    CHECK(tile_look(Domain::Climate, &autom).glyph == rgb::kGreen);
    CHECK(!tile_look(Domain::Climate, &coff).active);
    State smoke = st("on", "smoke"), motion = st("on", "motion"), clear = st("off", "smoke");
    CHECK(tile_look(Domain::BinarySensor, &smoke).glyph == rgb::kRed);
    CHECK(tile_look(Domain::BinarySensor, &motion).glyph == rgb::kAmber);
    CHECK(!tile_look(Domain::BinarySensor, &clear).active);
    State b15 = st("15", "battery"), b40 = st("40", "battery"), b80 = st("80", "battery"), temp = st("21.5", "temperature");
    CHECK(tile_look(Domain::Sensor, &b15).glyph == rgb::kRed);
    CHECK(tile_look(Domain::Sensor, &b40).glyph == rgb::kOrange);
    CHECK(tile_look(Domain::Sensor, &b80).glyph == rgb::kGreen);
    CHECK(!tile_look(Domain::Sensor, &temp).active);
    State open = st("open"), closed = st("closed");
    CHECK(tile_look(Domain::Cover, &open).glyph == rgb::kPurple);
    CHECK(!tile_look(Domain::Cover, &closed).active);
    State script = st("on");
    CHECK(tile_look(Domain::Script, &script).active);

    /* the words: never by colour alone */
    State light = st("on");
    light.brightness = 184;
    light.dimmable = true;
    CHECK_EQ(tile_word(Domain::Light, light, -1, "\xC2\xB0" "C"), std::string("On, 72 %"));
    light.brightness = -1;
    CHECK_EQ(tile_word(Domain::Light, light, -1, ""), std::string("On"));
    State onoff = st("on"); /* an on / off light reporting 255 */
    onoff.brightness = 255;
    CHECK_EQ(tile_word(Domain::Light, onoff, -1, ""), std::string("On"));
    CHECK_EQ(tile_word(Domain::Light, off, -1, ""), std::string("Off"));
    State t = st("21.53", "temperature");
    t.unit = "\xC2\xB0" "C";
    CHECK_EQ(tile_word(Domain::Sensor, t, 1, ""), std::string("21.5 \xC2\xB0" "C"));
    State pm = st("12", "pm25");
    pm.unit = "\xC2\xB5g/m\xC2\xB3";
    CHECK_EQ(tile_word(Domain::Sensor, pm, -1, ""), std::string("12 g/m")); /* only the degree sign is kept */
    CHECK_EQ(tile_word(Domain::Lock, locked, -1, ""), std::string("Locked"));
    CHECK_EQ(tile_word(Domain::Scene, st("2026-10-31T19:00:00+00:00"), -1, ""), std::string("Scene"));
    CHECK_EQ(tile_word(Domain::Script, st("off"), -1, ""), std::string("Script"));
    CHECK_EQ(tile_word(Domain::Script, st("on"), -1, ""), std::string("Running"));
    CHECK_EQ(tile_word(Domain::Light, gone, -1, ""), std::string("Unavailable"));
    CHECK_EQ(tile_word(Domain::Scene, gone, -1, ""), std::string("Unavailable"));
    CHECK_EQ(tile_word(Domain::Sensor, st("unknown"), -1, ""), std::string("Unknown"));
    State thermo = st("heat");
    thermo.current_temperature = 21.4;
    CHECK_EQ(tile_word(Domain::Climate, thermo, -1, "\xC2\xB0" "C"), std::string("Heat, 21.4 \xC2\xB0" "C"));
    State blind = st("open");
    blind.current_position = 60;
    CHECK_EQ(tile_word(Domain::Cover, blind, -1, ""), std::string("Open, 60 %"));
    blind.current_position = 100;
    CHECK_EQ(tile_word(Domain::Cover, blind, -1, ""), std::string("Open"));
    CHECK_EQ(tile_word(Domain::BinarySensor, st("on", "window"), -1, ""), std::string("Open"));
    CHECK_EQ(tile_unit("\xC2\xB0" "F"), std::string("\xC2\xB0" "F"));
    /* the entity page's rows keep the degree sign too (the lists keep ASCII) */
    CHECK_EQ(format_temperature(21.4, "\xC2\xB0" "C", true), std::string("21.4 \xC2\xB0" "C"));
    CHECK_EQ(format_temperature(21.4, "\xC2\xB0" "C"), std::string("21.4 C"));
    CHECK_EQ(format_temperature(21.0, "C", true), std::string("21 C"));

    CHECK(tile_action(Domain::Light) == TileAction::Toggle);
    CHECK(tile_action(Domain::InputBoolean) == TileAction::Toggle);
    CHECK(tile_action(Domain::Scene) == TileAction::Run);
    CHECK(tile_action(Domain::Lock) == TileAction::Lock);
    CHECK(tile_action(Domain::Cover) == TileAction::Cover);
    CHECK(tile_action(Domain::Climate) == TileAction::Details);
    CHECK(tile_action(Domain::Sensor) == TileAction::Details);

    /* the chevron: the page has more than the icon's action */
    CHECK(tile_has_more(Domain::Light, TileAction::Toggle, true));       /* the brightness */
    CHECK(!tile_has_more(Domain::Light, TileAction::Toggle, false));     /* an on / off light: only the toggle */
    CHECK(!tile_has_more(Domain::Switch, TileAction::Toggle, false));
    CHECK(!tile_has_more(Domain::Scene, TileAction::Run, false));
    CHECK(!tile_has_more(Domain::Lock, TileAction::Lock, false));
    CHECK(tile_has_more(Domain::Cover, TileAction::Cover, false));       /* open and close, the position */
    CHECK(tile_has_more(Domain::Climate, TileAction::Details, false));
    CHECK(tile_has_more(Domain::Sensor, TileAction::Details, false));
    CHECK(tile_has_more(Domain::BinarySensor, TileAction::Details, false));
}

static void test_icons()
{
    IconRule r;
    r.def = "mdi:a";
    r.state = {{"off", "mdi:b"}};
    r.range = {{0, "mdi:r0"}, {50, "mdi:r50"}};
    CHECK_EQ(pick_icon(r, "off"), std::string("mdi:b"));
    CHECK_EQ(pick_icon(r, "on"), std::string("mdi:a"));
    CHECK_EQ(pick_icon(r, "72"), std::string("mdi:r50"));
    CHECK_EQ(pick_icon(r, "10"), std::string("mdi:r0"));
    CHECK_EQ(pick_icon(r, "-5"), std::string("mdi:a")); /* below every step: the default */

    IconRules rules;
    CHECK(!rules.set_component(J("[]")));
    CHECK(rules.set_component(J("{\"resources\":{\"light\":{\"_\":{\"default\":\"mdi:lightbulb\",\"state\":{\"off\":\"mdi:lightbulb-off\"}}},"
                                "\"sensor\":{\"_\":{\"default\":\"mdi:eye\"},\"battery\":{\"default\":\"mdi:battery\",\"range\":{\"0\":\"mdi:battery-outline\",\"50\":\"mdi:battery-50\"}}},"
                                "\"media_player\":{\"_\":{\"default\":\"mdi:cast\"}}}}")));
    CHECK(rules.component("media_player", "") == nullptr); /* not a phase-1 domain: not kept */
    CHECK(rules.component("light", "") != nullptr);
    CHECK(rules.component("sensor", "battery") && rules.component("sensor", "battery")->range.size() == 2);
    CHECK(rules.component("sensor", "humidity") == rules.component("sensor", "")); /* no rule for the class: "_" */
    CHECK(rules.set_entity(J("{\"resources\":{\"hue\":{\"light\":{\"hue_light\":{\"default\":\"mdi:hue\"},\"other\":{\"default\":\"mdi:x\"}}}}}"),
                           {"hue|light|hue_light"}));
    CHECK(rules.entity("hue", "light", "hue_light") != nullptr);
    CHECK(rules.entity("hue", "light", "other") == nullptr); /* no entity of ours has that key: not kept */
    CHECK(rules.size() == 4);
    /* the cache's form round-trips */
    IconRules back;
    CHECK(back.from_json(J(rules.to_json())));
    CHECK(back == rules);
    CHECK_EQ(back.component("sensor", "battery")->range[1].second, std::string("mdi:battery-50"));

    /* the order: registry icon, state icon, translation key, battery level, domain rule, our table */
    RegEntry e;
    e.id = "light.k";
    e.icon = "mdi:ceiling-light";
    e.platform = "hue";
    e.translation_key = "hue_light";
    State s = st("off");
    s.icon = "mdi:lamp";
    auto c = icon_candidates("light.k", &e, &s, rules);
    CHECK(c.size() == 4); /* the rule's and our table's "lightbulb-off" once */
    CHECK_EQ(c[0], std::string("mdi:ceiling-light"));
    CHECK_EQ(c[1], std::string("mdi:lamp"));
    CHECK_EQ(c[2], std::string("mdi:hue"));
    CHECK_EQ(c[3], std::string("mdi:lightbulb-off"));
    e.icon.clear();
    s.icon.clear();
    c = icon_candidates("light.k", &e, &s, rules);
    CHECK_EQ(c[0], std::string("mdi:hue"));
    /* unknown names to the font (a custom set) fall through to the next source */
    e.icon = "phu:fancy";
    const auto known = [](const std::string &n) { return n.rfind("mdi:", 0) == 0 && n != "mdi:hue"; };
    CHECK_EQ(resolve_icon("light.k", &e, &s, rules, known), std::string("mdi:lightbulb-off"));
    /* the battery level comes before the domain's rule */
    State bat = st("37", "battery");
    c = icon_candidates("sensor.b", nullptr, &bat, rules);
    CHECK_EQ(c[0], std::string("mdi:battery-30"));
    CHECK_EQ(c[1], std::string("mdi:battery-outline")); /* the rule's range for 37 */
    /* no rule from Home Assistant: our own table, which changes with the state */
    IconRules none;
    State lon = st("on"), loff = st("off");
    CHECK_EQ(resolve_icon("light.x", nullptr, &lon, none, nullptr), std::string("mdi:lightbulb"));
    CHECK_EQ(resolve_icon("light.x", nullptr, &loff, none, nullptr), std::string("mdi:lightbulb-off"));
    State lk = st("locked"), ul = st("unlocked");
    CHECK_EQ(resolve_icon("lock.x", nullptr, &lk, none, nullptr), std::string("mdi:lock"));
    CHECK_EQ(resolve_icon("lock.x", nullptr, &ul, none, nullptr), std::string("mdi:lock-open-variant"));
    State gar = st("closed", "garage");
    CHECK_EQ(resolve_icon("cover.g", nullptr, &gar, none, nullptr), std::string("mdi:garage"));
    State b95 = st("97", "battery"), b3 = st("3", "battery");
    CHECK_EQ(fallback_icon(Domain::Sensor, &b95), std::string("mdi:battery"));
    CHECK_EQ(fallback_icon(Domain::Sensor, &b3), std::string("mdi:battery-outline"));
    CHECK_EQ(fallback_icon(Domain::Scene, nullptr), std::string("mdi:palette"));
    for (Domain d : {Domain::Light, Domain::Switch, Domain::InputBoolean, Domain::Scene, Domain::Script, Domain::Sensor,
                     Domain::BinarySensor, Domain::Cover, Domain::Lock, Domain::Climate})
        CHECK(!fallback_icon(d, nullptr).empty());
}

static void test_mdi_names()
{
    MdiNames bad;
    CHECK(!bad.open("/nonexistent/mdi-names.bin"));
    CHECK(bad.lookup("mdi:lightbulb") == 0);
    MdiNames m;
    CHECK(m.open(mdi_path()));
    CHECK(m.loaded());
    CHECK(m.size() == 13659); /* 7,447 icons and their aliases (an alias shared by icons once) */
    CHECK_EQ(m.version(), std::string("7.4.47"));
    CHECK(m.lookup("mdi:lightbulb") == 0xF0335u);
    CHECK(m.lookup("lightbulb") == 0xF0335u);
    CHECK(m.lookup("mdi:lightbulb-off") == 0xF0E4Fu);
    CHECK(m.lookup("mdi:lock-open-variant") == 0xF0FC6u);
    CHECK(m.lookup("mdi:ab-testing") == 0xF01C9u); /* the first name */
    CHECK(m.lookup("mdi:invite") == 0xF0010u);     /* an alias several icons share: the first of them */
    CHECK(m.lookup("mdi:no-such-icon") == 0);
    CHECK(m.lookup("phu:lightbulb") == 0); /* another icon set */
    CHECK(m.lookup("") == 0);
    CHECK(m.lookup("mdi:") == 0);
    /* every name of our own fallback table is in the font */
    for (const char *n : {"mdi:lightbulb", "mdi:toggle-switch", "mdi:toggle-switch-off", "mdi:palette", "mdi:script-text",
                          "mdi:eye", "mdi:thermometer", "mdi:battery-unknown", "mdi:battery-10", "mdi:battery-90",
                          "mdi:radiobox-blank", "mdi:checkbox-marked-circle", "mdi:window-shutter", "mdi:garage-open",
                          "mdi:lock-alert", "mdi:lock-clock", "mdi:thermostat", "mdi:help-circle-outline",
                          "mdi:smoke-detector-alert", "mdi:motion-sensor-off", "mdi:gate-open", "mdi:water-off"})
        CHECK(m.lookup(n) != 0);
    CHECK_EQ(utf8_of(0xF0335), std::string("\xF3\xB0\x8C\xB5"));
    CHECK_EQ(utf8_of(0xB0), std::string("\xC2\xB0"));
    CHECK(utf8_of(0).empty());
    CHECK(utf8_of(0xD800).empty());
}

static void test_layout_cache()
{
    char tmpl[] = "/tmp/ha-tiles-XXXXXX";
    const char *base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string dir = std::string(base) + "/home-assistant";
    std::vector<Area> areas;
    std::vector<RegEntry> entries;
    IconRules icons;
    CHECK(!store::load_layout(dir, areas, entries, icons));
    set_mdi_path(mdi_path());
    const MdiNames &names = mdi();
    CHECK(names.loaded());

    /* an engine online with a registry and icon rules writes the layout; a new one starts from it */
    SimBackend b(sim_world("home"), Settings(), [] {
        Credentials c;
        c.server = "http://homeassistant.local:8123";
        c.client_id = "http://192.168.1.42:41234/";
        c.refresh_token = "sim-refresh-1";
        return c;
    }(), {"light.living_ceiling", "light.kitchen", "switch.coffee_machine", "sensor.garage_power", "sensor.hall_motion_battery"},
                 1.0, false);
    b.advance(1500);
    View v = b.snapshot();
    CHECK(v.conn == Conn::Connected);
    CHECK(b.layout_saves() >= 1);
    CHECK(!b.saved_layout().empty());
    CHECK(b.engine().icon_rules().has_component());
    /* the frontend/get_icons requests: the phase-1 domains, then the integrations with translation keys */
    int comp = 0, ent = 0;
    for (const std::string &m : b.world().sent) {
        if (m.find("\"type\":\"frontend/get_icons\",\"category\":\"entity_component\",\"integration\":[\"binary_sensor\"") !=
            std::string::npos)
            ++comp;
        if (m.find("\"type\":\"frontend/get_icons\",\"category\":\"entity\",\"integration\":[\"demo\"]") != std::string::npos) ++ent;
    }
    CHECK(comp == 1 && ent == 1);
    /* the icons the tiles get, source by source */
    auto tile = [&v](const std::string &id) -> const EntityRow * {
        for (const EntityRow &r : v.favourites)
            if (r.id == id) return &r;
        return nullptr;
    };
    v = b.snapshot();
    CHECK(tile("light.kitchen") && tile("light.kitchen")->glyph == names.lookup("mdi:ceiling-light"));          /* registry */
    CHECK(tile("switch.coffee_machine") && tile("switch.coffee_machine")->glyph == names.lookup("mdi:coffee-maker")); /* attribute */
    CHECK(tile("sensor.garage_power") && tile("sensor.garage_power")->glyph == names.lookup("mdi:meter-electric")); /* key */
    CHECK(tile("light.living_ceiling") && tile("light.living_ceiling")->glyph == names.lookup("mdi:lightbulb"));    /* rule */
    CHECK(tile("sensor.hall_motion_battery") && tile("sensor.hall_motion_battery")->glyph == names.lookup("mdi:battery-10"));
    CHECK(tile("sensor.hall_motion_battery")->glyph_rgb == rgb::kRed);
    CHECK_EQ(tile("light.living_ceiling")->tile_value, std::string("On, 72 %"));
    CHECK(tile("light.living_ceiling")->dimmable && tile("light.living_ceiling")->active);
    CHECK(tile("light.living_ceiling")->tile_action == TileAction::Toggle);
    /* the light turned off: the glyph follows the state */
    b.send(Command::of(Command::Kind::Toggle, "light.living_ceiling"));
    v = b.snapshot();
    CHECK_EQ(tile("light.living_ceiling")->tile_value, std::string("...")); /* no optimistic flip */
    b.advance(300);
    v = b.snapshot();
    CHECK_EQ(tile("light.living_ceiling")->tile_value, std::string("Off"));
    CHECK(tile("light.living_ceiling")->glyph == names.lookup("mdi:lightbulb-off"));
    CHECK(!tile("light.living_ceiling")->active);
    /* the tabs */
    CHECK(v.tabs.size() == 7); /* Favourites, Bedroom, Garage, Hall, Kitchen, Living room, All */
    CHECK_EQ(v.tabs[1].title, std::string("Bedroom"));
    CHECK_EQ(v.tabs.back().title, std::string("All"));

    CHECK(store::save_layout(dir, b.saved_layout()));
    struct stat sb;
    CHECK(::stat((dir + "/layout.json").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
    CHECK(store::load_layout(dir, areas, entries, icons));
    CHECK(areas.size() == 5);
    CHECK(icons.has_component());
    bool kitchen_icon = false;
    for (const RegEntry &e : entries) kitchen_icon = kitchen_icon || (e.id == "light.kitchen" && e.icon == "mdi:ceiling-light");
    CHECK(kitchen_icon);
    for (const RegEntry &e : entries) CHECK(e.id != "light.hidden_strip" && e.id != "sensor.router_uptime");

    /* a start without a connection from that file: the tabs and the tiles, greyed */
    SimWorld down = sim_world("home");
    down.reachable = false;
    Credentials c;
    c.server = "http://homeassistant.local:8123";
    c.client_id = "http://192.168.1.42:41234/";
    c.refresh_token = "sim-refresh-1";
    SimBackend off(down, Settings(), c, {"light.kitchen", "lock.front_door"}, 1.0, false);
    off.engine().set_cached_layout(areas, entries, icons);
    off.advance(20000);
    v = off.snapshot();
    CHECK(v.conn == Conn::Offline);
    CHECK(v.tabs.size() == 7);
    CHECK(v.registry_loaded);
    CHECK(tile("light.kitchen") && tile("light.kitchen")->muted);
    CHECK_EQ(tile("light.kitchen")->name, std::string("Kitchen light"));
    CHECK_EQ(tile("light.kitchen")->tile_value, std::string("Offline"));
    CHECK(tile("light.kitchen")->glyph == names.lookup("mdi:ceiling-light"));
    CHECK(tile("lock.front_door")->confirm);

    /* an older server without frontend/get_icons: our own table */
    SimWorld old = sim_world("home");
    old.icons = false;
    SimBackend o(old, Settings(), c, {"light.reading_lamp"}, 1.0, false);
    o.advance(1500);
    CHECK(!o.engine().icon_rules().has_component());
    v = o.snapshot();
    CHECK(tile("light.reading_lamp") && tile("light.reading_lamp")->glyph == names.lookup("mdi:lightbulb-off"));

    /* a damaged or oversized file reads as nothing */
    CHECK(store::write_atomic(dir + "/layout.json", "{\"version\": 2}", 0600));
    CHECK(!store::load_layout(dir, areas, entries, icons));
    CHECK(!store::save_layout(dir, std::string(kMaxLayoutBytes + 1, ' ')));
    std::string cmd = std::string("rm -rf ") + base;
    (void)!std::system(cmd.c_str());
}

void test_tiles_group()
{
    test_columns_and_tabs();
    test_grid_and_focus();
    test_looks_and_words();
    test_icons();
    test_mdi_names();
    test_layout_cache();
}
