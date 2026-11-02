/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: 0.3.0's stored dashboard (study 053 P2): the importer (string and object rows, section / divider,
 * unknown rows and cards, hidden views and subviews, name / icon overrides, sections and panel views, strategies, caps,
 * the round trip), masonry (columns by mm, the shortest column), the toggle domains of the rows and the header toggle,
 * the forecast, the history and its buckets, the dashboard chosen (Automatic, explicit, Off, a map-only one never
 * offered), the cache (round trip, mode, the 1 MB cap), and the engine against the simulated Home Assistant's
 * owner-shaped dashboard (requests, subscriptions, forecast and history, the cards the page gets).
 */
#include "test_util.hpp"

#include "dash.hpp"
#include "engine.hpp"
#include "json.hpp"
#include "lovelace.hpp"
#include "mdi.hpp"
#include "sim.hpp"
#include "store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace ha;

namespace {

json::Value J(const std::string &text)
{
    json::Value v;
    if (!json::parse(text, v)) std::fprintf(stderr, "bad test JSON: %.80s\n", text.c_str());
    return v;
}

Dashboard imported(const std::string &text, Import *res = nullptr)
{
    Dashboard d;
    const Import r = import_lovelace(J(text), d);
    if (res) *res = r;
    return d;
}

Credentials creds()
{
    Credentials c;
    c.server = "http://homeassistant.local:8123";
    c.client_id = "http://192.168.1.42:41234/";
    c.refresh_token = "sim-refresh-1";
    return c;
}

int count_kind(const DashView &v, DashCard::Kind k)
{
    int n = 0;
    for (const DashCard &c : v.cards) n += c.kind == k;
    return n;
}

int entity_refs(const DashView &v)
{
    int n = 0;
    for (const DashCard &c : v.cards) {
        n += !c.entity.empty();
        for (const DashRow &r : c.rows) n += r.kind == DashRow::Kind::Entity;
    }
    return n;
}

const DashCardView *card_titled(const View &v, const std::string &title)
{
    for (const DashCardView &c : v.dash_cards)
        if (c.title == title) return &c;
    return nullptr;
}

const DashCardView *card_kind_of(const View &v, DashCard::Kind k, int nth = 0)
{
    for (const DashCardView &c : v.dash_cards)
        if (c.kind == k && nth-- == 0) return &c;
    return nullptr;
}

const DashRowView *row_of(const DashCardView *c, const std::string &id)
{
    if (!c) return nullptr;
    for (const DashRowView &r : c->rows)
        if (r.kind == DashRow::Kind::Entity && r.e.id == id) return &r;
    return nullptr;
}

std::string mdi_path()
{
    return std::string(CDK_APP_DIR) + "/root/usr/share/APPLaunch/share/home-assistant/mdi-names.bin";
}

} // namespace

static void test_import()
{
    /* the owner-shaped fixture (study 053 section 13's counts, invented names) */
    const SimWorld w = sim_world("dash");
    Import res = Import::Invalid;
    const Dashboard d = imported(w.default_config, &res);
    CHECK(res == Import::Ok);
    CHECK_EQ(d.title, std::string("Home"));
    CHECK(d.views.size() == 1);
    const DashView &v = d.views[0];
    CHECK_EQ(v.type, std::string("masonry")); /* no type written: masonry */
    CHECK(v.cards.size() == 12);
    CHECK(count_kind(v, DashCard::Kind::Entities) == 9);
    CHECK(count_kind(v, DashCard::Kind::Sensor) == 2);
    CHECK(count_kind(v, DashCard::Kind::Weather) == 1);
    CHECK(entity_refs(v) == 25);
    CHECK(view_entities(v).size() == 25);
    CHECK(supported_cards(d) == 12);
    /* a string row and an object row with name and icon overrides */
    CHECK(v.cards[0].rows[0].kind == DashRow::Kind::Entity);
    CHECK_EQ(v.cards[0].rows[0].entity, std::string("light.living_ceiling"));
    CHECK(v.cards[0].rows[0].name.empty() && v.cards[0].rows[0].icon.empty());
    CHECK_EQ(v.cards[0].rows[1].name, std::string("Lamp"));
    CHECK_EQ(v.cards[0].rows[1].icon, std::string("mdi:floor-lamp"));
    CHECK(v.cards[0].header_toggle);
    CHECK(!v.cards[3].header_toggle); /* show_header_toggle: false */
    /* section and divider rows; an unknown row type kept with its name; tap_action; secondary_info */
    CHECK(v.cards[6].rows[2].kind == DashRow::Kind::Section);
    CHECK_EQ(v.cards[6].rows[2].name, std::string("Outside"));
    CHECK(v.cards[7].rows[1].kind == DashRow::Kind::Divider);
    CHECK(v.cards[10].rows[1].kind == DashRow::Kind::Other);
    CHECK_EQ(v.cards[10].rows[1].type, std::string("weblink"));
    CHECK_EQ(v.cards[10].rows[1].name, std::string("Air quality report"));
    CHECK(v.cards[10].rows[0].tap == TapAction::Toggle);
    CHECK_EQ(v.cards[8].rows[0].secondary, std::string("last-changed"));
    CHECK(v.cards[11].title.empty());
    /* sensor and weather keys */
    CHECK(v.cards[4].kind == DashCard::Kind::Sensor && v.cards[4].graph && v.cards[4].hours == 24 && v.cards[4].detail == 1);
    CHECK_EQ(v.cards[4].title, std::string("Living room"));
    CHECK(v.cards[9].graph && v.cards[9].hours == 12 && v.cards[9].detail == 2);
    CHECK(v.cards[5].kind == DashCard::Kind::Weather && v.cards[5].forecast && v.cards[5].current);
    CHECK(view_forecasts(v) == std::vector<std::string>{"weather.home"});
    /* a sensor card without graph: none (Home Assistant draws a graph only for "line") */
    CHECK(!imported("{\"views\":[{\"cards\":[{\"type\":\"sensor\",\"entity\":\"sensor.x\"}]}]}").views[0].cards[0].graph);

    /* the map-only dashboard: imported, no supported card */
    const Dashboard map = imported(w.dashboard_configs.at("dashboard-map"));
    CHECK(map.views.size() == 1 && map.views[0].type == "panel");
    CHECK(map.views[0].cards[0].kind == DashCard::Kind::Unsupported);
    CHECK_EQ(map.views[0].cards[0].type, std::string("map"));
    CHECK(supported_cards(map) == 0);

    /* unknown cards and rows, an icon-only view, a panel view, a subview and a hidden view */
    const Dashboard mixed = imported(sim_world("dash-mixed").default_config);
    CHECK(mixed.views.size() == 5);
    const DashView &o = mixed.views[0];
    CHECK(o.cards[1].rows.size() == 6);
    CHECK(o.cards[1].rows[2].kind == DashRow::Kind::Other && o.cards[1].rows[2].type == "button");
    CHECK(o.cards[1].rows[3].kind == DashRow::Kind::Other && o.cards[1].rows[3].name == "All off");
    CHECK(o.cards[1].rows[4].kind == DashRow::Kind::Other && o.cards[1].rows[4].name.empty());
    CHECK(o.cards[0].kind == DashCard::Kind::Unsupported && o.cards[0].type == "thermostat");
    CHECK_EQ(o.cards[0].title, std::string("Heating"));
    CHECK_EQ(o.cards[2].type, std::string("custom:mushroom-light-card"));
    CHECK(mixed.views[1].title.empty() && mixed.views[1].icon == "mdi:floor-plan");
    CHECK(!mixed.views[1].cards[1].graph);           /* graph: none */
    CHECK(!mixed.views[1].cards[2].forecast);        /* show_forecast: false */
    CHECK(view_forecasts(mixed.views[1]).empty());
    CHECK(view_visible(mixed.views[0], "u1"));
    CHECK(view_visible(mixed.views[2], "u1"));
    CHECK(!view_visible(mixed.views[3], "u1"));      /* a subview: no tab */
    CHECK(!view_visible(mixed.views[4], "u1"));      /* visible: false */
    CHECK(view_cards(mixed.views[2]).size() == 1);   /* a panel: its first card */
    const Dashboard users = imported("{\"views\":[{\"title\":\"Mine\",\"visible\":[{\"user\":\"u1\"},{\"user\":\"u3\"}],"
                                     "\"cards\":[]}]}");
    CHECK(users.views[0].users.size() == 2);
    CHECK(view_visible(users.views[0], "u1"));
    CHECK(!view_visible(users.views[0], "u2"));
    CHECK(view_visible(users.views[0], ""));         /* the user not known yet (offline): shown */
    const Dashboard panel = imported("{\"views\":[{\"panel\":true,\"cards\":[{\"type\":\"map\"},{\"type\":\"entities\","
                                     "\"entities\":[\"light.a\"]}]}]}");
    CHECK_EQ(panel.views[0].type, std::string("panel")); /* the old panel: true */
    CHECK(view_cards(panel.views[0]).size() == 1 && supported_cards(panel) == 0);
    CHECK(view_entities(panel.views[0]).empty());

    /* a sections view: its cards flattened in order; a strategy view and a strategy dashboard */
    const Dashboard sections = imported("{\"views\":[{\"type\":\"sections\",\"sections\":[{\"type\":\"grid\",\"cards\":"
                                        "[{\"type\":\"heading\",\"heading\":\"Lights\"},{\"type\":\"entities\",\"entities\":"
                                        "[\"light.a\"]}]},{\"cards\":[{\"type\":\"tile\",\"entity\":\"light.b\"}]}]},"
                                        "{\"strategy\":{\"type\":\"areas\"},\"title\":\"Areas\"}]}");
    CHECK(sections.views[0].cards.size() == 3);
    CHECK(sections.views[0].cards[1].kind == DashCard::Kind::Entities);
    CHECK_EQ(sections.views[0].cards[2].type, std::string("tile"));
    CHECK(sections.views[1].cards.size() == 1 && sections.views[1].cards[0].type == "strategy");
    imported("{\"strategy\":{\"type\":\"original-states\"}}", &res);
    CHECK(res == Import::Generated);
    imported("[1,2]", &res);
    CHECK(res == Import::Invalid);
    imported("{\"title\":\"x\"}", &res);
    CHECK(res == Import::Invalid);

    /* wrong types and bad ids are skipped, unknown keys ignored, never a crash */
    const Dashboard odd = imported("{\"views\":[{\"cards\":[{\"type\":\"entities\",\"title\":7,\"entities\":"
                                   "[42,\"nodot\",{\"entity\":5},{\"name\":\"no entity\"},\"light.ok\",null,"
                                   "{\"entity\":\"switch.x\",\"tap_action\":\"toggle\",\"style\":{\"a\":1}}],"
                                   "\"state_color\":true,\"card_mod\":{}},\"notacard\",{\"type\":\"entities\",\"entities\":"
                                   "\"notalist\"},{\"type\":\"sensor\",\"entity\":\"bad\",\"hours_to_show\":\"x\","
                                   "\"detail\":9}]},\"notaview\"]}");
    CHECK(odd.views.size() == 1);
    CHECK(odd.views[0].cards.size() == 3);
    CHECK(odd.views[0].cards[0].title.empty());
    CHECK(odd.views[0].cards[0].rows.size() == 2);
    CHECK(odd.views[0].cards[0].rows[1].tap == TapAction::Default); /* not an object: the default */
    CHECK(odd.views[0].cards[1].rows.empty());
    CHECK(odd.views[0].cards[2].entity.empty() && odd.views[0].cards[2].hours == 24 && odd.views[0].cards[2].detail == 2);
    std::string many = "{\"views\":[{\"cards\":[{\"type\":\"entities\",\"entities\":[";
    for (int i = 0; i < 300; ++i) many += std::string(i ? "," : "") + "\"light.l" + std::to_string(i) + "\"";
    many += "]}]}]}";
    CHECK(imported(many).views[0].cards[0].rows.size() == kMaxDashRows);

    /* the round trip of the cache: import(export(d)) == d */
    CHECK(imported(lovelace_text(d)) == d);
    CHECK(imported(lovelace_text(mixed)) == mixed);
    CHECK(imported(lovelace_text(sections)) == sections);
    CHECK(imported(lovelace_text(users)) == users);
    CHECK(lovelace_text(d).size() < w.default_config.size());
}

static void test_masonry()
{
    /* clamp(floor(width_mm / 50), 1, 4) */
    CHECK(masonry_columns(640, 1130) == 1);  /* the deck: 56.6 mm */
    CHECK(masonry_columns(480, 1130) == 1);  /* 480x320 at the deck's density: 42.5 mm */
    CHECK(masonry_columns(480, 610) == 1);   /* the Pi 3A+'s 3.5" panel: 78.7 mm */
    CHECK(masonry_columns(800, 920) == 1);   /* HyperPixel 800x480: 87 mm */
    CHECK(masonry_columns(800, 741) == 2);   /* the Pi 4B's 5": 108 mm */
    CHECK(masonry_columns(1280, 1160) == 2); /* 1280x720 harness: 110 mm */
    CHECK(masonry_columns(1280, 860) == 2);  /* the tablet: 149 mm */
    CHECK(masonry_columns(480, 850) == 1);   /* portrait 480x800: 56 mm */
    CHECK(masonry_columns(1700, 860) == 3);  /* 198 mm */
    CHECK(masonry_columns(2000, 860) == 4);  /* 233 mm */
    CHECK(masonry_columns(4000, 860) == 4);  /* at most 4 */
    CHECK(masonry_columns(100, 1130) == 1);  /* at least 1 */
    CHECK(masonry_columns(565, 1130) == 1 && masonry_columns(1130, 1130) == 2); /* exactly 100 mm: 2 */
    /* each card into the shortest column so far, the leftmost of equals */
    std::vector<int> ys;
    CHECK(masonry_place({100, 50, 50, 50}, 2, 10, &ys) == (std::vector<int>{0, 1, 1, 0}));
    CHECK(ys == (std::vector<int>{0, 0, 60, 110}));
    CHECK(masonry_place({30, 30, 30}, 3, 5, &ys) == (std::vector<int>{0, 1, 2}));
    CHECK(masonry_place({10, 20, 30}, 1, 4, &ys) == (std::vector<int>{0, 0, 0}));
    CHECK(ys == (std::vector<int>{0, 14, 38}));
    CHECK(masonry_place({}, 2, 4, &ys).empty() && ys.empty());
    CHECK(masonry_place({5, 5}, 0, 0, &ys) == (std::vector<int>{0, 0})); /* 0 columns read as 1 */
}

static void test_toggles_forecast_history()
{
    for (const char *id : {"light.a", "switch.b", "fan.c", "input_boolean.d"}) CHECK(toggle_domain(id));
    for (const char *id : {"lock.a", "cover.garage", "scene.x", "script.y", "climate.z", "sensor.t", "weather.h",
                           "automation.a", "person.p", "lightx.a", "light"})
        CHECK(!toggle_domain(id));

    /* the forecast: five days at most, day names in the zone given */
    const json::Value ev = J("{\"type\":\"daily\",\"forecast\":["
                             "{\"datetime\":\"2026-11-02T23:30:00+00:00\",\"condition\":\"sunny\",\"temperature\":14.2,\"templow\":6.1},"
                             "{\"datetime\":\"2026-11-03T11:00:00+00:00\",\"condition\":\"rainy\",\"temperature\":9},"
                             "{\"datetime\":\"2026-11-04T11:00:00Z\",\"condition\":\"partlycloudy\",\"temperature\":11,\"templow\":\"x\"},"
                             "{\"datetime\":\"bad\",\"condition\":\"fog\"},"
                             "{\"datetime\":\"2026-11-06T11:00:00+01:00\",\"condition\":\"snowy\"},"
                             "{\"datetime\":\"2026-11-07T11:00:00+00:00\",\"condition\":\"cloudy\"},"
                             "{\"datetime\":\"2026-11-08T11:00:00+00:00\",\"condition\":\"cloudy\"}]}");
    std::vector<ForecastDay> days;
    CHECK(parse_forecast(ev, 0, days));
    CHECK(days.size() == 5);
    CHECK_EQ(days[0].day, std::string("Mon"));
    CHECK_EQ(days[0].condition, std::string("sunny"));
    CHECK(std::fabs(days[0].high - 14.2) < 1e-9 && std::fabs(days[0].low - 6.1) < 1e-9);
    CHECK_EQ(days[1].day, std::string("Tue"));
    CHECK(std::isnan(days[1].low));
    CHECK(std::isnan(days[2].low)); /* not a number */
    CHECK_EQ(days[2].day, std::string("Wed"));
    CHECK(days[3].day.empty() && days[3].condition == "fog");
    CHECK_EQ(days[4].day, std::string("Fri"));
    CHECK(parse_forecast(ev, 3600, days));
    CHECK_EQ(days[0].day, std::string("Tue")); /* 00:30 the next day an hour east */
    CHECK(parse_forecast(ev, 0, days, 2) && days.size() == 2);
    CHECK(!parse_forecast(J("{\"type\":\"daily\"}"), 0, days) && days.empty());
    CHECK(!parse_forecast(J("[1]"), 0, days));
    CHECK_EQ(weather_icon("partlycloudy"), std::string("mdi:weather-partly-cloudy"));
    CHECK_EQ(weather_icon("clear-night"), std::string("mdi:weather-night"));
    CHECK_EQ(weather_icon("exceptional"), std::string("mdi:alert-circle-outline"));
    CHECK(weather_icon("volcanic").empty());
    CHECK_EQ(weather_word("lightning-rainy"), std::string("Lightning, rainy"));
    CHECK_EQ(weather_word("windy-variant"), std::string("Windy"));
    CHECK_EQ(weather_word("dust-storm"), std::string("Dust storm"));
    CHECK_EQ(weather_word("unavailable"), std::string("Unavailable"));

    /* ISO times */
    CHECK_EQ(iso_utc(0), std::string("1970-01-01T00:00:00+00:00"));
    CHECK_EQ(iso_utc(1793613600), std::string("2026-11-02T10:00:00+00:00"));
    double t = 0;
    CHECK(parse_iso("2026-11-02T10:00:00+00:00", t) && t == 1793613600.0);
    CHECK(parse_iso("2026-11-02T12:00:00+02:00", t) && t == 1793613600.0);
    CHECK(parse_iso("2026-11-02 10:00:00.5Z", t) && std::fabs(t - 1793613600.5) < 1e-6);
    CHECK(parse_iso("2026-11-02T10:00:00", t) && t == 1793613600.0); /* no zone: UTC */
    CHECK(!parse_iso("2026-13-02T10:00:00Z", t));
    CHECK(!parse_iso("2026-11-02T10:00", t));
    CHECK(!parse_iso("2026-11-02T10:00:00+0200", t));

    /* history: numeric states in time order, "lc" when "lu" is absent */
    const json::Value hist = J("{\"sensor.t\":[{\"s\":\"20.0\",\"a\":{},\"lu\":10},{\"s\":\"unavailable\",\"lu\":20},"
                               "{\"s\":\"22\",\"lc\":30},{\"s\":\"21.5\",\"lu\":25.5},{\"s\":\"x1\",\"lu\":40},{\"lu\":50}],"
                               "\"sensor.u\":[]}");
    const std::vector<HistoryPoint> pts = parse_history(hist, "sensor.t");
    CHECK(pts.size() == 3);
    CHECK(pts[0].t == 10 && pts[0].v == 20 && pts[1].t == 25.5 && pts[1].v == 21.5 && pts[2].t == 30 && pts[2].v == 22);
    CHECK(parse_history(hist, "sensor.u").empty());
    CHECK(parse_history(hist, "sensor.missing").empty());
    /* buckets: averages, an empty bucket keeps the last value, NAN before the first */
    const std::vector<HistoryPoint> p2 = {{5, 10}, {10, 20}, {30, 30}, {80, 40}, {100, 50}};
    std::vector<double> b = downsample(p2, 0, 100, 4);
    CHECK(b.size() == 4);
    CHECK(b[0] == 15 && b[1] == 30 && b[2] == 30 && b[3] == 45); /* the end point counts in the last bucket */
    b = downsample({{60, 7}, {-5, 3}}, 0, 100, 4); /* not sorted: parse_history sorts, downsample expects order */
    b = downsample({{-5, 3}, {60, 7}}, 0, 100, 4);
    CHECK(b[0] == 3 && b[1] == 3 && b[2] == 7 && b[3] == 7); /* a point before the start: the state at the start */
    b = downsample({{60, 7}}, 0, 100, 4);
    CHECK(std::isnan(b[0]) && std::isnan(b[1]) && b[2] == 7 && b[3] == 7);
    CHECK(downsample(p2, 10, 10, 4).empty());
    CHECK(downsample(p2, 0, 10, 0).empty());
    CHECK(graph_points(24, 1) == 24);
    CHECK(graph_points(24, 2) == 100); /* 144, capped */
    CHECK(graph_points(12, 2) == 72);
    CHECK(graph_points(1, 1) == 2);
    CHECK(scale_points({1, 2, 3, NAN}) == (std::vector<int>{0, 500, 1000, -1}));
    CHECK(scale_points({4, 4}) == (std::vector<int>{500, 500}));
    CHECK(scale_points({NAN}) == (std::vector<int>{-1}));

    /* the words of rows of any domain */
    State s;
    s.state = "home";
    CHECK_EQ(entity_word("person.a", &s, -1, "\xC2\xB0" "C"), std::string("Home"));
    s.state = "not_home";
    CHECK_EQ(entity_word("person.a", &s, -1, "\xC2\xB0" "C"), std::string("Away"));
    CHECK_EQ(entity_word("device_tracker.p", &s, -1, "\xC2\xB0" "C"), std::string("Away"));
    s.state = "partlycloudy";
    CHECK_EQ(entity_word("weather.h", &s, -1, ""), std::string("Partly cloudy"));
    s.state = "on";
    CHECK_EQ(entity_word("fan.f", &s, -1, ""), std::string("On"));
    s.state = "8.73";
    s.unit = "\xC2\xB0" "C";
    CHECK_EQ(entity_word("sensor.o", &s, 1, ""), std::string("8.7 \xC2\xB0" "C"));
    CHECK_EQ(entity_word("input_number.x", &s, -1, ""), std::string("8.73 \xC2\xB0" "C"));
    s.state = "above_horizon";
    s.unit.clear();
    CHECK_EQ(entity_word("sun.sun", &s, -1, ""), std::string("Above horizon"));
    s.state = "unavailable";
    CHECK_EQ(entity_word("person.a", &s, -1, ""), std::string("Unavailable"));
    CHECK(entity_word("person.a", nullptr, -1, "").empty());
    CHECK_EQ(card_temperature(14.2, "\xC2\xB0" "C"), std::string("14.2 \xC2\xB0" "C"));
    CHECK_EQ(card_temperature(14.0, "\xC2\xB0" "F"), std::string("14 \xC2\xB0" "F"));
    CHECK(card_temperature(NAN, "").empty());
}

static void test_selection()
{
    DashChoice def, map, energy;
    def.key = kDashDefault;
    def.title = "Home";
    def.loaded = true;
    def.supported = 12;
    map.key = "dashboard-map";
    map.title = "Map";
    map.loaded = true;
    map.supported = 0;
    energy.key = "dashboard-energy";
    energy.title = "Energy";
    energy.loaded = true;
    energy.supported = 1;
    std::vector<DashChoice> owner{def, map};
    /* Automatic: the default dashboard; the map-only one is never offered nor shown */
    CHECK_EQ(resolve_dashboard(kDashAuto, owner, ""), std::string("lovelace"));
    CHECK_EQ(resolve_dashboard("", owner, ""), std::string("lovelace"));
    CHECK_EQ(resolve_dashboard(kDashAuto, owner, "lovelace"), std::string("lovelace"));
    CHECK_EQ(resolve_dashboard(kDashAuto, owner, "home"), std::string("lovelace")); /* the generated Overview: not here */
    CHECK(offered_dashboards(owner).size() == 1 && offered_dashboards(owner)[0].key == "lovelace");
    CHECK_EQ(wanted_dashboard("dashboard-map", owner, ""), std::string("dashboard-map"));
    CHECK(resolve_dashboard("dashboard-map", owner, "").empty());
    CHECK_EQ(resolve_dashboard(kDashAuto, owner, "dashboard-map"), std::string()); /* the default panel is the map */
    /* Off */
    CHECK(resolve_dashboard(kDashOff, owner, "").empty());
    CHECK(wanted_dashboard(kDashOff, owner, "").empty());
    /* explicit, and the system's default panel naming a listed dashboard */
    std::vector<DashChoice> three{def, map, energy};
    CHECK_EQ(resolve_dashboard("dashboard-energy", three, ""), std::string("dashboard-energy"));
    CHECK_EQ(resolve_dashboard(kDashAuto, three, "dashboard-energy"), std::string("dashboard-energy"));
    CHECK_EQ(resolve_dashboard("dashboard-gone", three, ""), std::string("lovelace")); /* no longer listed: Automatic */
    /* the cycle of Settings > Dashboard: Automatic, each offered dashboard, Off */
    CHECK_EQ(next_dashboard_setting(kDashAuto, three), std::string("lovelace"));
    CHECK_EQ(next_dashboard_setting("lovelace", three), std::string("dashboard-energy"));
    CHECK_EQ(next_dashboard_setting("dashboard-energy", three), std::string("off"));
    CHECK_EQ(next_dashboard_setting(kDashOff, three), std::string("auto"));
    CHECK_EQ(next_dashboard_setting("dashboard-map", three), std::string("auto"));
    CHECK_EQ(next_dashboard_setting("", owner), std::string("lovelace"));
    /* a generated, missing, too large or not yet loaded default: the tiles */
    std::vector<DashChoice> gen{def};
    gen[0].generated = true;
    CHECK(resolve_dashboard(kDashAuto, gen, "").empty());
    gen[0] = def;
    gen[0].missing = true;
    CHECK(resolve_dashboard(kDashAuto, gen, "").empty());
    gen[0] = def;
    gen[0].too_large = true;
    CHECK(resolve_dashboard(kDashAuto, gen, "").empty());
    CHECK_EQ(wanted_dashboard(kDashAuto, gen, ""), std::string("lovelace"));
    gen[0] = def;
    gen[0].loaded = false;
    CHECK(resolve_dashboard(kDashAuto, gen, "").empty() && offered_dashboards(gen).empty());
    CHECK(resolve_dashboard(kDashAuto, {}, "").empty());
}

static void test_engine_dashboard()
{
    set_mdi_path(mdi_path());
    const MdiNames &names = mdi();
    /* the owner-shaped world: the default dashboard shown, the map-only one known but not offered */
    SimBackend b(sim_world("dash"), Settings(), creds(), {"light.living_ceiling"}, 1.0, false);
    b.advance(1500);
    View v = b.snapshot();
    CHECK(v.conn == Conn::Connected);
    CHECK_EQ(b.engine().dashboard_key(), std::string("lovelace"));
    CHECK(b.engine().dashboard_choices().size() == 2);
    CHECK(b.world().config_reads == 2);
    CHECK(v.dash && v.dash_tabs.size() == 1);
    CHECK_EQ(v.dash_title, std::string("Home"));
    CHECK(v.dash_choices.size() == 1 && v.dash_choices[0].first == "lovelace");
    CHECK_EQ(v.dash_auto_title, std::string("Home"));
    /* the tabs: the view first, then Favourites, the three areas with entities, All */
    CHECK(v.tabs.size() == 6);
    CHECK(v.tabs[0].kind == Tab::Kind::View && v.tabs[0].view == 0);
    CHECK_EQ(v.tabs[0].title, std::string("Home"));
    CHECK(v.tabs[0].glyph == names.lookup("mdi:home"));
    CHECK_EQ(v.tabs[0].key(), std::string("view:0"));
    CHECK(v.tabs[1].kind == Tab::Kind::Favourites);
    CHECK(v.dash_view == -1 && v.dash_cards.empty()); /* no view shown yet */
    CHECK(b.dashboard_saves() >= 1);
    for (const std::string &m : b.world().sent) {
        CHECK(m.find("lovelace/config/save") == std::string::npos);
        CHECK(m.find("lovelace/dashboards/create") == std::string::npos);
        CHECK(m.find("weather/subscribe_forecast") == std::string::npos); /* only while its view shows */
    }

    /* the view shown: its entities subscribed (with the favourites), the forecast subscribed, the history asked */
    b.send(Command::set_view(false, "", "", 0));
    b.advance(600);
    v = b.snapshot();
    CHECK(v.dash_view == 0 && v.dash_cards.size() == 12 && !v.dash_panel);
    CHECK(b.engine().subscribed().size() == 25);
    CHECK(b.engine().subscribed().count("weather.home") && b.engine().subscribed().count("person.sam"));
    CHECK(b.world().forecast_subscribes == 1);
    CHECK(b.world().history_reads == 2);
    int forecast_msgs = 0, history_msgs = 0;
    for (const std::string &m : b.world().sent) {
        if (m.find("\"type\":\"weather/subscribe_forecast\",\"entity_id\":\"weather.home\",\"forecast_type\":\"daily\"}") !=
            std::string::npos)
            ++forecast_msgs;
        if (m.find("\"type\":\"history/history_during_period\",\"start_time\":\"") != std::string::npos &&
            m.find("\"entity_ids\":[\"sensor.living_temperature\"],\"minimal_response\":true,\"no_attributes\":true,"
                   "\"significant_changes_only\":false}") != std::string::npos)
            ++history_msgs;
    }
    CHECK(forecast_msgs == 1 && history_msgs == 1);

    const DashCardView *living = card_titled(v, "Living room");
    CHECK(living && living->kind == DashCard::Kind::Entities && living->rows.size() == 4);
    /* the header toggle: lights and the switch, never the scene; on while one is on */
    CHECK(living->header_toggle && living->header_on);
    CHECK(living->toggle_ids == (std::vector<std::string>{"light.living_ceiling", "light.reading_lamp", "switch.tv_plug"}));
    const DashRowView *lamp = row_of(living, "light.reading_lamp");
    CHECK(lamp && lamp->e.name == "Lamp" && lamp->e.glyph == names.lookup("mdi:floor-lamp")); /* overrides */
    CHECK(lamp->toggle && !lamp->e.on && lamp->word == "Off");
    CHECK(row_of(living, "light.living_ceiling")->word == "On, 72 %");
    CHECK(!row_of(living, "scene.evening")->toggle && row_of(living, "scene.evening")->word == "Scene");
    const DashCardView *doors = card_titled(v, "Doors");
    CHECK(doors && !doors->header_toggle); /* show_header_toggle: false, and locks / covers never */
    CHECK(row_of(doors, "lock.front_door")->e.confirm && row_of(doors, "lock.front_door")->word == "Locked");
    CHECK(row_of(doors, "cover.garage_door")->e.confirm && !row_of(doors, "cover.garage_door")->toggle);
    const DashCardView *bedroom = card_titled(v, "Bedroom");
    CHECK(bedroom && bedroom->toggle_ids == std::vector<std::string>{"light.bedside"}); /* not the blind nor the script */
    const DashCardView *climate = card_titled(v, "Climate");
    CHECK(climate && !climate->header_toggle && climate->rows[2].kind == DashRow::Kind::Section &&
          climate->rows[2].label == "Outside");
    CHECK_EQ(row_of(climate, "sensor.outdoor_temperature")->word, std::string("8.7 \xC2\xB0" "C"));
    CHECK_EQ(row_of(climate, "climate.thermostat")->word, std::string("Heat, 21.4 \xC2\xB0" "C"));
    const DashCardView *hall = card_titled(v, "Hall");
    CHECK(hall && hall->rows[1].kind == DashRow::Kind::Divider);
    CHECK(row_of(hall, "light.porch")->e.unavailable);
    CHECK(hall->toggle_ids == std::vector<std::string>{"input_boolean.guest_mode"}); /* the unavailable light: out */
    const DashCardView *family = card_titled(v, "Family");
    CHECK(family && row_of(family, "person.alex")->word == "Home" && row_of(family, "person.sam")->word == "Away");
    CHECK_EQ(row_of(family, "person.alex")->e.name, std::string("Alex"));
    CHECK(row_of(family, "person.alex")->e.glyph == names.lookup("mdi:account"));
    const DashCardView *air = card_titled(v, "Air");
    CHECK(air && row_of(air, "fan.bedroom_fan")->toggle && row_of(air, "fan.bedroom_fan")->tap == TapAction::Toggle);
    CHECK(air->rows[1].kind == DashRow::Kind::Other && air->rows[1].label == "Air quality report");
    /* the sensor cards: the value, the unit, the points of the history */
    const DashCardView *temp = card_kind_of(v, DashCard::Kind::Sensor, 0);
    CHECK(temp && temp->e.id == "sensor.living_temperature" && temp->e.name == "Living room");
    CHECK_EQ(temp->value, std::string("21.5"));
    CHECK_EQ(temp->unit, std::string("\xC2\xB0" "C"));
    CHECK(temp->graph && temp->spark.size() == 24);
    CHECK(*std::max_element(temp->spark.begin(), temp->spark.end()) == 1000);
    const DashCardView *power = card_kind_of(v, DashCard::Kind::Sensor, 1);
    CHECK(power && power->spark.size() == 72 && power->value == "12" && power->unit == "W");
    /* the weather card: the condition, the temperature, five days */
    const DashCardView *weather = card_kind_of(v, DashCard::Kind::Weather);
    CHECK(weather && weather->condition == "Partly cloudy" && weather->value == "14.2");
    CHECK_EQ(weather->unit, std::string("\xC2\xB0" "C"));
    CHECK_EQ(weather->e.name, std::string("Forecast home"));
    CHECK(weather->big_glyph == names.lookup("mdi:weather-partly-cloudy"));
    CHECK(weather->days.size() == 5);
    CHECK_EQ(weather->days[0].high, std::string("14\xC2\xB0"));
    CHECK_EQ(weather->days[0].low, std::string("6\xC2\xB0"));
    CHECK(weather->days[2].glyph == names.lookup("mdi:weather-rainy"));
    CHECK(!weather->days[4].day.empty());

    /* the header toggle: one call per domain with the list; a fan's row toggles */
    b.send(Command::set_group(living->toggle_ids, !living->header_on));
    b.advance(300);
    CHECK(std::find(b.world().calls.begin(), b.world().calls.end(),
                    "light.turn_off light.living_ceiling,light.reading_lamp") != b.world().calls.end());
    CHECK(std::find(b.world().calls.begin(), b.world().calls.end(), "switch.turn_off switch.tv_plug") != b.world().calls.end());
    v = b.snapshot();
    CHECK(!card_titled(v, "Living room")->header_on);
    b.send(Command::set_group({"lock.front_door", "cover.garage_door", "light.bedside"}, true)); /* locks never */
    b.advance(300);
    CHECK(b.world().calls.back() == "light.turn_on light.bedside");
    b.send(Command::of(Command::Kind::Toggle, "fan.bedroom_fan"));
    b.advance(300);
    CHECK(b.world().calls.back() == "fan.toggle fan.bedroom_fan");
    v = b.snapshot();
    CHECK(row_of(card_titled(v, "Air"), "fan.bedroom_fan")->e.on);
    CHECK(row_of(card_titled(v, "Air"), "fan.bedroom_fan")->e.glyph_rgb == rgb::kCyan);

    /* another tab: the forecast unsubscribed, the view's entities no longer subscribed */
    const int sends = static_cast<int>(b.world().sent.size());
    b.send(Command::set_view(false, "", "", -1));
    b.advance(200);
    CHECK(b.engine().forecast_subscriptions().empty());
    bool unsub = false;
    for (std::size_t i = static_cast<std::size_t>(sends); i < b.world().sent.size(); ++i)
        unsub = unsub || b.world().sent[i].find("\"type\":\"unsubscribe_events\"") != std::string::npos;
    CHECK(unsub);
    CHECK(b.engine().subscribed().size() == 1);
    CHECK(b.snapshot().dash_cards.empty());
    /* back: subscribed again, the history not asked again within 5 minutes */
    b.send(Command::set_view(false, "", "", 0));
    b.advance(300);
    CHECK(b.world().forecast_subscribes == 2 && b.world().history_reads == 2);
    b.advance(5 * 60 * 1000 + 11000, 1000);
    CHECK(b.world().history_reads == 4);

    /* Settings > Dashboard: Off shows the tiles; Automatic again */
    b.send(Command::of(Command::Kind::SetDashboard, "off"));
    v = b.snapshot();
    CHECK(!v.dash && v.tabs[0].kind == Tab::Kind::Favourites && v.tabs.size() == 5);
    CHECK_EQ(b.engine().settings().dashboard, std::string("off"));
    CHECK(b.engine().forecast_subscriptions().empty());
    b.send(Command::of(Command::Kind::SetDashboard, "nonsense"));
    CHECK_EQ(b.engine().settings().dashboard, std::string("auto"));
    CHECK(b.snapshot().dash);

    /* the mixed world: unsupported cards drawn as such, a missing entity, the icon-only and panel views */
    SimBackend m(sim_world("dash-mixed"), Settings(), creds(), {}, 1.0, false);
    m.advance(1500);
    v = m.snapshot();
    CHECK(v.dash && v.dash_tabs.size() == 3); /* not the subview, not the hidden view */
    CHECK(v.dash_tabs[1].title.empty() && v.dash_tabs[1].glyph == names.lookup("mdi:floor-plan"));
    CHECK(v.tabs[1].title.empty() && v.tabs[1].glyph != 0);
    CHECK(v.dash_choices.size() == 2); /* the default and Energy, not the map */
    m.send(Command::set_view(false, "", "", 0));
    m.advance(500);
    v = m.snapshot();
    CHECK(v.dash_cards.size() == 5);
    CHECK(v.dash_cards[0].kind == DashCard::Kind::Unsupported && v.dash_cards[0].title == "Heating");
    CHECK(v.dash_cards[2].kind == DashCard::Kind::Unsupported && v.dash_cards[2].type == "custom:mushroom-light-card");
    CHECK(v.dash_cards[1].rows[2].kind == DashRow::Kind::Other && v.dash_cards[1].rows[2].label == "Restart router");
    CHECK(v.dash_cards[1].rows[4].label == "conditional"); /* no name: its type */
    CHECK(!v.dash_cards[4].header_toggle);                /* a lock and a cover only */
    m.send(Command::set_view(false, "", "", 1));
    m.advance(500);
    v = m.snapshot();
    const DashRowView *missing = row_of(card_kind_of(v, DashCard::Kind::Entities), "light.missing_lamp");
    CHECK(missing && !missing->available && missing->word == "Not available" && missing->e.unavailable);
    CHECK(card_kind_of(v, DashCard::Kind::Sensor)->spark.empty() && !card_kind_of(v, DashCard::Kind::Sensor)->graph);
    CHECK(card_kind_of(v, DashCard::Kind::Weather)->days.empty());
    CHECK(m.world().forecast_subscribes == 0); /* show_forecast: false */
    m.send(Command::set_view(false, "", "", 2));
    m.advance(300);
    v = m.snapshot();
    CHECK(v.dash_panel && v.dash_cards.size() == 1 && v.dash_cards[0].type == "map");
    m.send(Command::set_view(false, "", "", 3)); /* a subview: no cards */
    m.advance(300);
    CHECK(m.snapshot().dash_view == -1);

    /* a home without a stored dashboard (config_not_found): the tiles as in 0.2.0 */
    SimBackend h(sim_world("home"), Settings(), creds(), {}, 1.0, false);
    h.advance(1500);
    CHECK(!h.snapshot().dash && h.engine().dashboard_key().empty());
    CHECK(h.snapshot().tabs[0].kind == Tab::Kind::Favourites);
    /* an older server without the lovelace commands: the same */
    SimWorld old = sim_world("dash");
    old.lovelace = false;
    SimBackend o(old, Settings(), creds(), {}, 1.0, false);
    o.advance(1500);
    CHECK(!o.snapshot().dash && o.snapshot().conn == Conn::Connected);
    /* the system's default panel names a listed dashboard: Automatic shows it */
    SimWorld dp = sim_world("dash-mixed");
    dp.default_panel = "dashboard-energy";
    SimBackend e(dp, Settings(), creds(), {}, 1.0, false);
    e.advance(1500);
    CHECK_EQ(e.engine().dashboard_key(), std::string("dashboard-energy"));
    CHECK(e.snapshot().dash_tabs.size() == 1 && e.snapshot().dash_tabs[0].title == "Power");
    /* the setting kept in settings.json chooses at start */
    Settings off;
    off.dashboard = "off";
    SimBackend f(sim_world("dash"), off, creds(), {}, 1.0, false);
    f.advance(1500);
    CHECK(!f.snapshot().dash && f.snapshot().dash_choices.size() == 1);
    /* a weather entity without a daily forecast: the current weather only */
    SimWorld nf = sim_world("dash");
    nf.forecasts = false;
    SimBackend n(nf, Settings(), creds(), {}, 1.0, false);
    n.advance(1500);
    n.send(Command::set_view(false, "", "", 0));
    n.advance(500);
    v = n.snapshot();
    CHECK(card_kind_of(v, DashCard::Kind::Weather)->days.empty());
    CHECK(card_kind_of(v, DashCard::Kind::Weather)->condition == "Partly cloudy");
    CHECK(n.engine().forecast_subscriptions().empty());
}

static void test_dashboard_cache()
{
    const std::string base = "/tmp/ha-test-dash-" + std::to_string(::getpid());
    const std::string dir = base + "/home-assistant";
    SimBackend b(sim_world("dash"), Settings(), creds(), {}, 1.0, false);
    b.advance(1500);
    b.send(Command::set_view(false, "", "", 0));
    b.advance(500);
    const std::string text = b.saved_dashboard();
    CHECK(!text.empty() && text == b.engine().dashboard_text());
    CHECK(text.find("\"key\": \"lovelace\"") != std::string::npos);
    CHECK(text.find("\"person.sam\": \"Sam\"") != std::string::npos); /* names as last seen */
    CHECK(text.size() < kMaxDashBytes);
    CHECK(store::save_dashboard(dir, text));
    struct stat sb;
    CHECK(::stat((dir + "/dashboard.json").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
    std::string back;
    CHECK(store::load_dashboard(dir, back) && back == text);

    /* a start without a connection from that file: the same tabs and cards, greyed, the names kept */
    SimWorld down = sim_world("dash");
    down.reachable = false;
    SimBackend off(down, Settings(), creds(), {}, 1.0, false);
    CHECK(off.engine().set_cached_dashboard(back));
    off.send(Command::set_view(false, "", "", 0));
    off.advance(20000);
    View v = off.snapshot();
    CHECK(v.conn == Conn::Offline);
    CHECK(v.dash && v.dash_cards.size() == 12 && v.tabs[0].kind == Tab::Kind::View);
    const DashRowView *sam = row_of(card_titled(v, "Family"), "person.sam");
    CHECK(sam && sam->e.name == "Sam" && sam->e.muted && sam->word == "Offline");
    CHECK(v.dash_choices.size() == 1 && v.dash_choices[0].second == "Home");
    CHECK(off.engine().dashboard_text() == back); /* nothing new to save */
    /* the round trip */
    SimBackend again(down, Settings(), creds(), {}, 1.0, false);
    CHECK(again.engine().set_cached_dashboard(off.engine().dashboard_text()));
    CHECK(again.engine().dashboard_text() == back);
    /* the world's own cache text (the harness's dash-offline world) reads too */
    SimBackend sim(down, Settings(), creds(), {}, 1.0, false);
    CHECK(sim.engine().set_cached_dashboard(sim_cached_dashboard(sim_world("dash"))));
    CHECK(sim.snapshot().dash);
    /* Off chosen meanwhile: the cache shows no dashboard */
    Settings tiles;
    tiles.dashboard = "off";
    SimBackend t(down, tiles, creds(), {}, 1.0, false);
    CHECK(t.engine().set_cached_dashboard(back));
    CHECK(!t.snapshot().dash);

    /* damaged, other version, larger than 1 MB: nothing */
    CHECK(!off.engine().set_cached_dashboard("{\"version\": 2}"));
    CHECK(!off.engine().set_cached_dashboard("not json"));
    CHECK(!off.engine().set_cached_dashboard(""));
    CHECK(!store::save_dashboard(dir, std::string(kMaxDashBytes + 1, ' ')));
    CHECK(store::write_atomic(dir + "/dashboard.json", std::string(kMaxDashBytes + 10, ' '), 0600));
    CHECK(!store::load_dashboard(dir, back));

    /* a stored dashboard over 1 MB: not shown, a message, the tiles */
    SimWorld huge = sim_world("dash");
    std::string config = "{\"views\":[{\"cards\":[{\"type\":\"entities\",\"entities\":[";
    for (int i = 0; config.size() < kMaxDashBytes + 1000; ++i)
        config += std::string(i ? "," : "") + "{\"entity\":\"light.l" + std::to_string(i) + "\",\"name\":\"A light with a name\"}";
    config += "]}]}]}";
    huge.default_config = config;
    SimBackend big(huge, Settings(), creds(), {}, 1.0, false);
    big.advance(1500);
    v = big.snapshot();
    CHECK(!v.dash && big.engine().dashboard_key().empty());
    CHECK(v.message_error && v.message.find("larger than 1 MB") != std::string::npos);
    CHECK(v.tabs[0].kind == Tab::Kind::Favourites);

    /* settings.json keeps the choice */
    Settings s;
    s.dashboard = "dashboard-energy";
    CHECK(store::save_settings(dir, s));
    CHECK_EQ(store::load_settings(dir).dashboard, std::string("dashboard-energy"));
    CHECK(store::write_atomic(dir + "/settings.json", "{\"server\": \"x:1\"}", 0644));
    CHECK_EQ(store::load_settings(dir).dashboard, std::string("auto")); /* a 0.2.0 file: Automatic */
    std::string cmd = std::string("rm -rf ") + base;
    (void)!std::system(cmd.c_str());
}

void test_lovelace_group()
{
    test_import();
    test_masonry();
    test_toggles_forecast_history();
    test_selection();
    test_engine_dashboard();
    test_dashboard_cache();
}
