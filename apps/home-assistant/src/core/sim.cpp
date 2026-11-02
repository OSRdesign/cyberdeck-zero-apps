/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the simulated Home Assistant (see sim.hpp).
 */
#include "sim.hpp"

#include "dash.hpp"
#include "json.hpp"
#include "text.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <time.h>

namespace ha {

namespace {

constexpr const char *kSimState = "00112233445566778899aabbccddeeff";
constexpr const char *kSimRefresh = "sim-refresh-1";

std::int64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

SimEntity ent(const std::string &id, const std::string &name, const std::string &area, const std::string &state)
{
    SimEntity e;
    e.id = id;
    e.name = name;
    e.area = area;
    e.state = state;
    return e;
}

std::string form_value(const std::string &body, const std::string &key)
{
    const auto q = parse_query(body);
    auto it = q.find(key);
    return it == q.end() ? std::string() : it->second;
}

bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

void home_world(SimWorld &w)
{
    w.areas = {{"living_room", "Living room"}, {"kitchen", "Kitchen"}, {"bedroom", "Bedroom"}, {"hall", "Hall"},
               {"garage", "Garage"}};
    w.device_area = {{"dev_thermostat", "living_room"}, {"dev_fridge", "kitchen"}, {"dev_garage", "garage"}};
    SimEntity e;
    e = ent("light.living_ceiling", "Ceiling light", "living_room", "on");
    e.dimmable = true;
    e.brightness = 184;
    w.entities.push_back(e);
    w.entities.push_back(ent("light.reading_lamp", "Reading lamp", "living_room", "off"));
    w.entities.push_back(ent("switch.tv_plug", "TV plug", "living_room", "on"));
    w.entities.push_back(ent("scene.evening", "Evening", "living_room", "2026-10-31T19:00:00+00:00"));
    e = ent("sensor.living_temperature", "Temperature", "living_room", "21.53");
    e.unit = "\xC2\xB0" "C";
    e.device_class = "temperature";
    e.precision = 1;
    w.entities.push_back(e);
    e = ent("sensor.living_humidity", "Humidity", "living_room", "48");
    e.unit = "%";
    e.device_class = "humidity";
    w.entities.push_back(e);
    e = ent("binary_sensor.living_window", "Window", "living_room", "off");
    e.device_class = "window";
    w.entities.push_back(e);
    e = ent("climate.thermostat", "Thermostat", "", "heat");
    e.device = "dev_thermostat";
    e.current_temperature = 21.4;
    e.target_temperature = 21;
    w.entities.push_back(e);
    e = ent("light.kitchen", "Kitchen light", "kitchen", "off");
    e.dimmable = true;
    e.icon = "mdi:ceiling-light"; /* the user's own icon in the registry */
    w.entities.push_back(e);
    e = ent("switch.coffee_machine", "Coffee machine", "kitchen", "off");
    e.attr_icon = "mdi:coffee-maker"; /* the integration's icon attribute */
    w.entities.push_back(e);
    e = ent("sensor.fridge", "Fridge", "", "4.1");
    e.device = "dev_fridge";
    e.unit = "\xC2\xB0" "C";
    w.entities.push_back(e);
    e = ent("binary_sensor.kitchen_motion", "Motion", "kitchen", "on");
    e.device_class = "motion";
    w.entities.push_back(e);
    w.entities.push_back(ent("script.good_night", "Good night", "bedroom", "off"));
    e = ent("light.bedside", "Bedside lamp", "bedroom", "on");
    e.dimmable = true;
    e.brightness = 77;
    w.entities.push_back(e);
    e = ent("cover.bedroom_blind", "Blind", "bedroom", "open");
    e.device_class = "shade";
    e.position = 60;
    w.entities.push_back(e);
    w.entities.push_back(ent("lock.front_door", "Front door", "hall", "locked"));
    e = ent("lock.back_door", "Back door", "hall", "locked");
    e.fail_service = "unlock";
    w.entities.push_back(e);
    e = ent("binary_sensor.front_door_contact", "Front door contact", "hall", "off");
    e.device_class = "door";
    w.entities.push_back(e);
    w.entities.push_back(ent("light.porch", "Porch light", "hall", "unavailable"));
    e = ent("cover.garage_door", "Garage door", "", "closed");
    e.device = "dev_garage";
    e.device_class = "garage";
    w.entities.push_back(e);
    e = ent("sensor.garage_power", "Power", "garage", "12.0");
    e.unit = "W";
    e.precision = 0;
    e.translation_key = "power_meter"; /* the integration's translation-key icon (frontend/get_icons entity) */
    w.entities.push_back(e);
    e = ent("sensor.hall_motion_battery", "Motion sensor battery", "hall", "15");
    e.unit = "%";
    e.device_class = "battery";
    w.entities.push_back(e);
    w.entities.push_back(ent("input_boolean.guest_mode", "Guest mode", "", "off"));
    e = ent("sensor.router_uptime", "Router uptime", "hall", "12");
    e.category = true; /* diagnostic: not shown */
    w.entities.push_back(e);
    e = ent("light.hidden_strip", "Hidden strip", "kitchen", "off");
    e.hidden = true;
    w.entities.push_back(e);
    w.entities.push_back(ent("media_player.tv", "TV", "living_room", "on")); /* not a phase 1 domain */
}

void big_world(SimWorld &w)
{
    home_world(w);
    w.areas.push_back({"workshop", "Workshop and long-named storage room at the back of the garden"});
    static const char *kinds[] = {"light", "switch", "sensor", "binary_sensor", "scene"};
    for (int i = 1; i <= 300; ++i) {
        char id[64], name[160];
        const char *k = kinds[i % 5];
        std::snprintf(id, sizeof id, "%s.workshop_%03d", k, i);
        std::snprintf(name, sizeof name, "Workbench %03d very long device name that will not fit on one row", i);
        SimEntity e = ent(id, name, "workshop", i % 3 ? "on" : "off");
        if (std::string(k) == "sensor") {
            e.state = std::to_string(100 + i) + ".5";
            e.unit = "W";
        }
        if (std::string(k) == "scene") e.state = "2026-10-31T19:00:00+00:00";
        if (std::string(k) == "light") e.dimmable = true, e.brightness = (i * 7) % 255;
        w.entities.push_back(e);
    }
}

/* ------------------------------------------------------------------ the stored dashboards (0.3.0, study 053 P2) */

/* The owner's dashboard as the probe of study 053 section 13 counted it (HA 2026.10.4): the default dashboard is a
 * stored config, one masonry view, 12 cards (9 entities, 2 sensor, 1 weather-forecast), 25 entity references, only
 * mdi: icons, nesting depth 1, about 1 KB; a second dashboard is one map card in a panel view. The names are invented. */
const char *kOwnerDashboard = R"JSON({"title": "Home", "views": [{"title": "Home", "path": "home", "icon": "mdi:home", "cards": [
  {"type": "entities", "title": "Living room", "entities": ["light.living_ceiling",
    {"entity": "light.reading_lamp", "name": "Lamp", "icon": "mdi:floor-lamp"}, "switch.tv_plug", "scene.evening"]},
  {"type": "entities", "title": "Kitchen", "entities": ["light.kitchen", "switch.coffee_machine", "binary_sensor.kitchen_motion"]},
  {"type": "entities", "title": "Bedroom", "entities": ["light.bedside", "cover.bedroom_blind", "script.good_night"]},
  {"type": "entities", "title": "Doors", "show_header_toggle": false, "entities": ["lock.front_door", "lock.back_door", "cover.garage_door"]},
  {"type": "sensor", "entity": "sensor.living_temperature", "name": "Living room", "graph": "line"},
  {"type": "weather-forecast", "entity": "weather.home", "show_forecast": true},
  {"type": "entities", "title": "Climate", "entities": ["climate.thermostat", "sensor.living_humidity",
    {"type": "section", "label": "Outside"}, "sensor.outdoor_temperature"]},
  {"type": "entities", "title": "Hall", "entities": ["light.porch", {"type": "divider"}, "input_boolean.guest_mode"]},
  {"type": "entities", "title": "Family", "entities": [{"entity": "person.alex", "secondary_info": "last-changed"}, "person.sam"]},
  {"type": "sensor", "entity": "sensor.garage_power", "graph": "line", "detail": 2, "hours_to_show": 12},
  {"type": "entities", "title": "Air", "entities": [{"entity": "fan.bedroom_fan", "tap_action": {"action": "toggle"}},
    {"type": "weblink", "name": "Air quality report", "url": "https://example.invalid/air"}]},
  {"type": "entities", "entities": ["sensor.hall_motion_battery"]}
]}]})JSON";

const char *kMapDashboard =
    R"JSON({"views": [{"type": "panel", "title": "Map", "path": "map", "cards": [{"type": "map", "entities": ["person.alex", "person.sam"]}]}]})JSON";

/* unsupported cards and rows, an icon-only view, a panel view with a map, a subview and a hidden view */
const char *kMixedDashboard = R"JSON({"title": "House", "views": [
  {"title": "Overview", "path": "overview", "cards": [
    {"type": "thermostat", "entity": "climate.thermostat", "title": "Heating"},
    {"type": "entities", "title": "Lights", "entities": ["light.living_ceiling", "light.kitchen",
      {"type": "button", "name": "Restart router", "tap_action": {"action": "perform-action"}},
      {"type": "call-service", "name": "All off", "action": "light.turn_off"},
      {"type": "conditional", "conditions": [], "row": {"entity": "light.porch"}},
      "light.bedside"]},
    {"type": "custom:mushroom-light-card", "entity": "light.kitchen"},
    {"type": "history-graph", "title": "Power", "entities": ["sensor.garage_power"]},
    {"type": "entities", "title": "Locks", "entities": ["lock.front_door", "cover.garage_door"]}]},
  {"icon": "mdi:floor-plan", "path": "plan", "cards": [
    {"type": "entities", "entities": ["person.alex", "light.missing_lamp"]},
    {"type": "sensor", "entity": "sensor.outdoor_temperature", "graph": "none"},
    {"type": "weather-forecast", "entity": "weather.home", "show_forecast": false}]},
  {"title": "Map", "path": "map", "type": "panel", "cards": [{"type": "map", "entities": ["person.alex"]}]},
  {"title": "Details", "path": "details", "subview": true, "cards": [{"type": "entities", "entities": ["light.kitchen"]}]},
  {"title": "Hidden", "path": "hidden", "visible": false, "cards": [{"type": "entities", "entities": ["light.kitchen"]}]}
]})JSON";

const char *kEnergyDashboard =
    R"JSON({"title": "Energy", "views": [{"title": "Power", "cards": [{"type": "sensor", "entity": "sensor.garage_power", "graph": "line"}]}]})JSON";

/* long names everywhere, six views (the tab bar scrolls) */
const char *kLongDashboard = R"JSON({"title": "A dashboard with a really long title for the deck", "views": [
  {"title": "Living room and kitchen downstairs", "cards": [
    {"type": "entities", "title": "Everything in the living room next to the big window", "entities": [
      {"entity": "light.living_ceiling", "name": "Ceiling light above the dining table by the window"},
      {"entity": "switch.tv_plug", "name": "Television and sound bar power plug behind the cabinet"},
      {"entity": "sensor.living_temperature", "name": "Temperature of the living room near the sofa"},
      {"type": "section", "label": "A section label that is much longer than the card is wide"},
      {"entity": "lock.front_door", "name": "Front door lock of the main entrance on the street side"},
      {"type": "weblink", "name": "A web link row with a long name that cannot be opened here"}]},
    {"type": "sensor", "entity": "sensor.garage_power", "name": "Power drawn by the garage workshop and the freezer", "graph": "line"},
    {"type": "weather-forecast", "entity": "weather.home", "name": "Weather at the holiday cottage by the sea"},
    {"type": "custom:a-very-long-custom-card-type-name-from-hacs", "title": "A custom card with a long title that is not supported"}]},
  {"title": "Bedrooms upstairs and the attic", "cards": [{"type": "entities", "entities": ["light.bedside"]}]},
  {"title": "Garden, shed and greenhouse", "cards": [{"type": "entities", "entities": ["sensor.outdoor_temperature"]}]},
  {"title": "Garage and workshop", "cards": [{"type": "entities", "entities": ["cover.garage_door"]}]},
  {"title": "Security cameras and alarms", "cards": [{"type": "entities", "entities": ["lock.back_door"]}]},
  {"title": "Energy", "cards": [{"type": "entities", "entities": ["sensor.garage_power"]}]}
]})JSON";

void dash_entities(SimWorld &w)
{
    SimEntity e = ent("sensor.outdoor_temperature", "Outdoor temperature", "", "8.7");
    e.unit = "\xC2\xB0" "C";
    e.device_class = "temperature";
    e.precision = 1;
    w.entities.push_back(e);
    e = ent("person.alex", "Alex", "", "home");
    e.platform = "person";
    w.entities.push_back(e);
    e = ent("person.sam", "Sam", "", "not_home");
    e.platform = "person";
    w.entities.push_back(e);
    e = ent("fan.bedroom_fan", "Bedroom fan", "bedroom", "off");
    w.entities.push_back(e);
    e = ent("weather.home", "Forecast home", "", "partlycloudy");
    e.target_temperature = 14.2;
    e.temperature_unit = "\xC2\xB0" "C";
    e.platform = "met";
    w.entities.push_back(e);
}

void dash_world(SimWorld &w)
{
    home_world(w);
    /* the owner's install has three areas: the other entities have none here ("No room") */
    w.areas = {{"living_room", "Living room"}, {"kitchen", "Kitchen"}, {"bedroom", "Bedroom"}};
    dash_entities(w);
    w.default_config = kOwnerDashboard;
    w.dashboards = {{"dashboard-map", "Map"}};
    w.dashboard_configs["dashboard-map"] = kMapDashboard;
}

void long_names(SimWorld &w)
{
    for (SimEntity &e : w.entities)
        if (e.id == "light.bedside") e.name = "Bedside lamp on the left of the double bed upstairs";
        else if (e.id == "cover.garage_door") e.name = "Garage door to the street with the remote control";
        else if (e.id == "lock.back_door") e.name = "Back door to the garden behind the kitchen";
        else if (e.id == "sensor.outdoor_temperature") e.name = "Outdoor temperature in the shade of the north wall";
        else if (e.id == "weather.home") e.name = "Forecast for home from the national weather service";
}

} // namespace

SimEntity *SimWorld::find(const std::string &id)
{
    for (SimEntity &e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

SimWorld sim_world(const std::string &name)
{
    SimWorld w;
    if (name == "empty") return w;
    if (name == "big") big_world(w);
    else if (name == "dash" || name == "dash-offline" || name == "dash-mixed" || name == "dash-long") dash_world(w);
    else home_world(w);
    if (name == "dash-mixed") {
        w.default_config = kMixedDashboard;
        w.dashboards = {{"dashboard-energy", "Energy"}, {"dashboard-map", "Map"}};
        w.dashboard_configs["dashboard-energy"] = kEnergyDashboard;
    }
    if (name == "dash-long") {
        w.default_config = kLongDashboard;
        long_names(w);
    }
    if (name == "connect-unreachable" || name == "connecting" || name == "dash-offline") w.reachable = false;
    return w;
}

std::string sim_cached_dashboard(const SimWorld &w)
{
    std::string names;
    for (const SimEntity &e : w.entities) {
        if (supported(domain_of(e.id))) continue; /* the registry has those */
        names += std::string(names.empty() ? "" : ", ") + json::quote(e.id) + ": " + json::quote(e.name);
    }
    return "{\"version\": 1, \"key\": \"lovelace\", \"default_panel\": \"\", \"user_id\": \"u1\", \"choices\": "
           "[{\"key\": \"lovelace\", \"title\": \"Home\", \"supported\": 12}], \"names\": {" +
           names + "}, \"config\": " + w.default_config + "}\n";
}

/* --------------------------------------------------------------------------------------------- the backend */

SimBackend::SimBackend(SimWorld world, Settings settings, Credentials creds, std::vector<std::string> favourites,
                       double speed, bool real_time, std::map<std::string, std::string> names)
    : world_(std::move(world)), engine_(std::move(settings), creds, favourites), speed_(speed > 0 ? speed : 1.0),
      real_time_(real_time), saved_creds_(creds), creds_file_(creds.valid()), saved_favs_(favourites),
      saved_names_(names)
{
    engine_.set_cached_names(std::move(names));
    if (creds.valid()) issued_refresh_ = creds.refresh_token;
    real_last_ = mono_ms();
    demo_started_ = now_;
    engine_.start(now_);
    run_actions();
}

std::int64_t SimBackend::wall() const
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

View SimBackend::snapshot()
{
    return engine_.view(now_, wall());
}

void SimBackend::send(const Command &c)
{
    engine_.on_command(c, now_);
    run_actions();
}

void SimBackend::run_actions()
{
    for (int guard = 0; guard < 1000; ++guard) {
        std::vector<Action> acts = engine_.take_actions();
        if (acts.empty()) return;
        for (const Action &a : acts) exec(a);
    }
}

void SimBackend::poll()
{
    if (real_time_) {
        const std::int64_t real = mono_ms();
        now_ += static_cast<std::int64_t>((real - real_last_) * speed_);
        real_last_ = real;
    }
    if (drop_real_ms_ && !drop_at_ && engine_.online() && engine_.view(now_, wall()).registry_loaded)
        drop_at_ = now_ + static_cast<std::int64_t>(drop_real_ms_ * speed_);
    if (drop_at_ && now_ >= drop_at_) {
        drop_at_ = 0;
        drop_real_ms_ = 0;
        world_.reachable = false;
        speed_ = speed_after_;
        drop_link();
    }
    if (!world_.phone.empty() && !phone_done_ && listening_ && !engine_.view(now_, wall()).qr_url.empty()) {
        if (!qr_seen_at_) qr_seen_at_ = now_;
        else if (now_ - qr_seen_at_ >= 1000) {
            phone_done_ = true;
            phone(world_.phone);
        }
    }
    deliver();
    engine_.on_tick(now_, wall());
    run_actions();
}

void SimBackend::advance(std::int64_t ms, std::int64_t step)
{
    const bool rt = real_time_;
    real_time_ = false;
    for (std::int64_t t = 0; t < ms; t += step) {
        now_ += step;
        poll();
    }
    real_time_ = rt;
}

void SimBackend::deliver()
{
    if (inbox_.empty()) return;
    std::deque<std::string> batch;
    batch.swap(inbox_);
    for (const std::string &entry : batch) {
        const std::size_t bar = entry.find('|');
        if (bar == std::string::npos || std::atoi(entry.substr(0, bar).c_str()) != link_serial_) continue; /* old link */
        const std::string m = entry.substr(bar + 1);
        if (m == "__close__") {
            link_open_ = authed_ = false;
            sub_id_ = 0;
            sub_ids_.clear();
            engine_.on_ws_closed("closed by Home Assistant", now_);
        } else if (link_open_) {
            engine_.on_ws_message(m, now_, wall());
        }
        run_actions();
    }
    /* answers to what the engine sent meanwhile arrive at the next poll */
}

void SimBackend::exec(const Action &a)
{
    switch (a.kind) {
    case Action::Kind::HttpGet:
        if (ends_with(a.url, "/auth/providers")) {
            ++world_.provider_checks;
            if (world_.reachable)
                engine_.on_http(a.tag, 200, "{\"providers\":[{\"name\":\"Home Assistant Local\",\"type\":\"homeassistant\"}]}",
                                "", now_);
            else engine_.on_http(a.tag, 0, "", "Could not connect to server", now_);
        } else {
            engine_.on_http(a.tag, 404, "", "", now_);
        }
        return;
    case Action::Kind::HttpPost: {
        if (!world_.reachable) {
            engine_.on_http(a.tag, 0, "", "Could not connect to server", now_);
            return;
        }
        if (ends_with(a.url, "/auth/revoke")) {
            ++world_.revokes;
            if (form_value(a.body, "token") == issued_refresh_) issued_refresh_.clear();
            engine_.on_http(a.tag, 200, "", "", now_);
            return;
        }
        if (!ends_with(a.url, "/auth/token")) {
            engine_.on_http(a.tag, 404, "", "", now_);
            return;
        }
        const std::string grant = form_value(a.body, "grant_type");
        const std::string access = "sim-access-" + std::to_string(++token_serial_);
        if (grant == "authorization_code") {
            ++world_.exchanges;
            if (form_value(a.body, "code") != "sim-code" || form_value(a.body, "client_id").empty()) {
                engine_.on_http(a.tag, 400, "{\"error\":\"invalid_request\",\"error_description\":\"Invalid code\"}", "",
                                now_);
                return;
            }
            issued_refresh_ = kSimRefresh;
            engine_.on_http(a.tag, 200,
                            "{\"access_token\":\"" + access + "\",\"token_type\":\"Bearer\",\"refresh_token\":\"" +
                                issued_refresh_ + "\",\"expires_in\":1800,\"ha_auth_provider\":\"homeassistant\"}",
                            "", now_);
            return;
        }
        if (grant == "refresh_token") {
            ++world_.refreshes;
            if (world_.refuse_refresh || form_value(a.body, "refresh_token") != issued_refresh_ || issued_refresh_.empty()) {
                engine_.on_http(a.tag, 400, "{\"error\":\"invalid_grant\"}", "", now_);
                return;
            }
            engine_.on_http(a.tag, 200,
                            "{\"access_token\":\"" + access + "\",\"token_type\":\"Bearer\",\"expires_in\":1800}", "",
                            now_);
            return;
        }
        engine_.on_http(a.tag, 400, "{\"error\":\"unsupported_grant_type\"}", "", now_);
        return;
    }
    case Action::Kind::WsOpen:
        ++world_.ws_opens;
        if (!world_.reachable) {
            engine_.on_ws_open(false, "Could not connect to server", now_);
            return;
        }
        link_open_ = true;
        authed_ = false;
        inbox_.clear();
        ++link_serial_;
        engine_.on_ws_open(true, "", now_);
        out("{\"type\":\"auth_required\",\"ha_version\":\"" + world_.ha_version + "\"}");
        return;
    case Action::Kind::WsSend:
        world_.sent.push_back(a.body);
        if (link_open_) ws_receive(a.body);
        return;
    case Action::Kind::WsClose:
        link_open_ = authed_ = false;
        sub_id_ = 0;
        sub_ids_.clear();
        inbox_.clear();
        ++link_serial_;
        return;
    case Action::Kind::ListenStart:
        listening_ = true;
        engine_.on_listen(true, "192.168.1.42", 41234, "192.168.1.10", kSimState, "", now_);
        return;
    case Action::Kind::ListenStop: listening_ = false; return;
    case Action::Kind::SaveCredentials:
        saved_creds_ = engine_.credentials();
        creds_file_ = true;
        return;
    case Action::Kind::DeleteCredentials:
        saved_creds_ = Credentials();
        creds_file_ = false;
        return;
    case Action::Kind::SaveSettings: return;
    case Action::Kind::SaveFavourites: saved_favs_ = engine_.favourites(); return;
    case Action::Kind::SaveNames: saved_names_ = engine_.cached_names(); return;
    case Action::Kind::SaveLayout:
        saved_layout_ = engine_.layout_text();
        ++layout_saves_;
        return;
    case Action::Kind::SaveDashboard:
        saved_dashboard_ = engine_.dashboard_text();
        ++dashboard_saves_;
        return;
    }
}

std::string SimBackend::compressed(const SimEntity &e) const
{
    std::string a = "\"friendly_name\":" + json::quote(e.name);
    if (!e.attr_icon.empty()) a += ",\"icon\":" + json::quote(e.attr_icon);
    if (!e.unit.empty()) a += ",\"unit_of_measurement\":" + json::quote(e.unit);
    if (!e.device_class.empty()) a += ",\"device_class\":" + json::quote(e.device_class);
    if (e.id.rfind("light.", 0) == 0) {
        a += e.dimmable ? ",\"supported_color_modes\":[\"brightness\"]" : ",\"supported_color_modes\":[\"onoff\"]";
        if (e.state == "on" && e.brightness >= 0) a += ",\"brightness\":" + std::to_string(e.brightness);
    }
    if (e.current_temperature > -999) a += ",\"current_temperature\":" + json::number(e.current_temperature);
    if (e.target_temperature > -999) a += ",\"temperature\":" + json::number(e.target_temperature);
    if (e.position >= 0) a += ",\"current_position\":" + std::to_string(e.position);
    if (!e.temperature_unit.empty()) a += ",\"temperature_unit\":" + json::quote(e.temperature_unit);
    return "{\"s\":" + json::quote(e.state) + ",\"a\":{" + a + "},\"c\":\"01SIM\",\"lc\":" +
           std::to_string(wall() - 600) + ".25}";
}

void SimBackend::push_state_change(const SimEntity &e, bool removed_brightness)
{
    if (!sub_id_ || !sub_ids_.count(e.id)) return;
    std::string plus = "\"s\":" + json::quote(e.state) + ",\"lc\":" + std::to_string(wall()) + ".5";
    if (e.id.rfind("light.", 0) == 0 && e.state == "on" && e.brightness >= 0)
        plus += ",\"a\":{\"brightness\":" + std::to_string(e.brightness) + "}";
    std::string change = "{\"+\":{" + plus + "}";
    if (removed_brightness) change += ",\"-\":{\"a\":[\"brightness\"]}";
    change += "}";
    out("{\"id\":" + std::to_string(sub_id_) + ",\"type\":\"event\",\"event\":{\"c\":{" +
                     json::quote(e.id) + ":" + change + "}}}");
}

void SimBackend::ws_receive(const std::string &text)
{
    json::Value m;
    if (!json::parse(text, m)) return;
    const std::string type = m.get("type") ? m.get("type")->as_string() : std::string();
    const int id = m.get("id") ? static_cast<int>(m.get("id")->as_int(0)) : 0;
    const std::string sid = std::to_string(id);
    auto result = [&](const std::string &r) {
        out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":true,\"result\":" + r + "}");
    };
    if (type == "auth") {
        ++world_.auths;
        const std::string token = m.get("access_token") ? m.get("access_token")->as_string() : std::string();
        if (world_.reject_access || token.rfind("sim-access-", 0) != 0) {
            world_.reject_access = false;
            out("{\"type\":\"auth_invalid\",\"message\":\"Invalid access token or password\"}");
            out("__close__");
            return;
        }
        authed_ = true;
        out("{\"type\":\"auth_ok\",\"ha_version\":\"" + world_.ha_version + "\"}");
        return;
    }
    if (!authed_) return;
    if (type == "supported_features" || type == "unsubscribe_events") {
        result("null");
    } else if (type == "ping") {
        out("{\"id\":" + sid + ",\"type\":\"pong\"}");
    } else if (type == "get_config") {
        result("{\"location_name\":\"Maison\",\"unit_system\":{\"temperature\":\"\xC2\xB0" "C\",\"length\":\"km\"},"
               "\"version\":\"" + world_.ha_version + "\"}");
    } else if (type == "auth/current_user") {
        result("{\"id\":\"u1\",\"name\":" + json::quote(world_.user) + ",\"is_owner\":false,\"is_admin\":false}");
    } else if (type == "config/area_registry/list") {
        std::string r = "[";
        for (std::size_t i = 0; i < world_.areas.size(); ++i)
            r += std::string(i ? "," : "") + "{\"area_id\":" + json::quote(world_.areas[i].first) +
                 ",\"name\":" + json::quote(world_.areas[i].second) + ",\"floor_id\":null}";
        result(r + "]");
    } else if (type == "config/device_registry/list") {
        std::string r = "[";
        bool first = true;
        for (const auto &d : world_.device_area) {
            r += std::string(first ? "" : ",") + "{\"id\":" + json::quote(d.first) + ",\"area_id\":" +
                 json::quote(d.second) + ",\"name\":" + json::quote(d.first) + ",\"name_by_user\":null}";
            first = false;
        }
        result(r + "]");
    } else if (type == "config/entity_registry/list_for_display") {
        std::string r = "{\"entity_categories\":{\"0\":\"config\",\"1\":\"diagnostic\"},\"entities\":[";
        bool first = true;
        for (const SimEntity &e : world_.entities) {
            r += first ? "" : ",";
            first = false;
            r += "{\"ei\":" + json::quote(e.id) + ",\"pl\":" + json::quote(e.platform) + ",\"en\":" + json::quote(e.name);
            if (!e.area.empty()) r += ",\"ai\":" + json::quote(e.area);
            if (!e.device.empty()) r += ",\"di\":" + json::quote(e.device);
            if (!e.icon.empty()) r += ",\"ic\":" + json::quote(e.icon);
            if (!e.translation_key.empty()) r += ",\"tk\":" + json::quote(e.translation_key);
            if (e.hidden) r += ",\"hb\":true";
            if (e.category) r += ",\"ec\":1";
            if (e.precision >= 0) r += ",\"dp\":" + std::to_string(e.precision);
            r += "}";
        }
        result(r + "]}");
    } else if (type == "subscribe_entities") {
        sub_id_ = id;
        sub_ids_.clear();
        if (const json::Value *ids = m.get("entity_ids"))
            for (const json::Value &v : ids->items) sub_ids_.insert(v.as_string());
        result("null");
        std::string a = "{";
        bool first = true;
        for (const SimEntity &e : world_.entities) {
            if (!sub_ids_.count(e.id)) continue;
            a += std::string(first ? "" : ",") + json::quote(e.id) + ":" + compressed(e);
            first = false;
        }
        out("{\"id\":" + sid + ",\"type\":\"event\",\"event\":{\"a\":" + a + "}}}");
    } else if (type == "frontend/get_icons" && world_.icons) {
        /* the test fixture's own few rules, in the shape Home Assistant serves them */
        const std::string category = m.get("category") ? m.get("category")->as_string() : "";
        if (category == "entity_component")
            result("{\"resources\":{"
                   "\"light\":{\"_\":{\"default\":\"mdi:lightbulb\",\"state\":{\"off\":\"mdi:lightbulb-off\"}}},"
                   "\"switch\":{\"_\":{\"default\":\"mdi:toggle-switch-variant\",\"state\":{\"off\":\"mdi:toggle-switch-variant-off\"}}},"
                   "\"input_boolean\":{\"_\":{\"default\":\"mdi:check-circle-outline\",\"state\":{\"off\":\"mdi:close-circle-outline\"}}},"
                   "\"scene\":{\"_\":{\"default\":\"mdi:palette\"}},"
                   "\"script\":{\"_\":{\"default\":\"mdi:script-text\"}},"
                   "\"lock\":{\"_\":{\"default\":\"mdi:lock\",\"state\":{\"unlocked\":\"mdi:lock-open-variant\",\"jammed\":\"mdi:lock-alert\"}}},"
                   "\"cover\":{\"_\":{\"default\":\"mdi:window-shutter-open\",\"state\":{\"closed\":\"mdi:window-shutter\"}},"
                   "\"garage\":{\"default\":\"mdi:garage-open\",\"state\":{\"closed\":\"mdi:garage\"}}},"
                   "\"climate\":{\"_\":{\"default\":\"mdi:thermostat\"}},"
                   "\"sensor\":{\"_\":{\"default\":\"mdi:eye\"},\"temperature\":{\"default\":\"mdi:thermometer\"},"
                   "\"humidity\":{\"default\":\"mdi:water-percent\"}},"
                   "\"binary_sensor\":{\"_\":{\"default\":\"mdi:radiobox-blank\",\"state\":{\"on\":\"mdi:checkbox-marked-circle\"}},"
                   "\"window\":{\"default\":\"mdi:window-closed\",\"state\":{\"on\":\"mdi:window-open\"}},"
                   "\"motion\":{\"default\":\"mdi:motion-sensor-off\",\"state\":{\"on\":\"mdi:motion-sensor\"}},"
                   "\"door\":{\"default\":\"mdi:door-closed\",\"state\":{\"on\":\"mdi:door-open\"}}},"
                   "\"media_player\":{\"_\":{\"default\":\"mdi:cast\"}}}}");
        else if (category == "entity")
            result("{\"resources\":{\"demo\":{\"sensor\":{\"power_meter\":{\"default\":\"mdi:meter-electric\"},"
                   "\"unused_key\":{\"default\":\"mdi:help\"}}}}}");
        else
            out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"invalid_format\"}}");
    } else if (world_.lovelace && type == "lovelace/dashboards/list") {
        std::string r = "[";
        for (std::size_t i = 0; i < world_.dashboards.size(); ++i)
            r += std::string(i ? "," : "") + "{\"id\":\"d" + std::to_string(i) + "\",\"url_path\":" +
                 json::quote(world_.dashboards[i].first) + ",\"title\":" + json::quote(world_.dashboards[i].second) +
                 ",\"icon\":null,\"mode\":\"storage\",\"require_admin\":false,\"show_in_sidebar\":true}";
        result(r + "]");
    } else if (world_.lovelace && type == "lovelace/config") {
        ++world_.config_reads;
        const json::Value *u = m.get("url_path");
        std::string config = world_.default_config;
        if (u && !u->is_null()) {
            auto it = world_.dashboard_configs.find(u->as_string());
            config = it == world_.dashboard_configs.end() ? std::string() : it->second;
        }
        if (config.empty())
            out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":"
                "\"config_not_found\",\"message\":\"No config found.\"}}");
        else result(config);
    } else if (type == "frontend/get_system_data") {
        result(world_.default_panel.empty() ? "{\"value\":null}"
                                            : "{\"value\":{\"default_panel\":" + json::quote(world_.default_panel) + "}}");
    } else if (world_.forecasts && type == "weather/subscribe_forecast") {
        ++world_.forecast_subscribes;
        const std::string entity = m.get("entity_id") ? m.get("entity_id")->as_string() : "";
        if (!world_.find(entity)) {
            out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":"
                "\"invalid_entity_id\",\"message\":\"Weather entity not found\"}}");
            return;
        }
        result("null");
        static const char *conds[] = {"sunny", "partlycloudy", "rainy", "cloudy", "pouring", "snowy", "fog"};
        static const double highs[] = {14.2, 12.0, 9.5, 11.1, 8.0, 3.5, 6.0}, lows[] = {6.1, 5.0, 4.2, 3.9, 2.0, -1.5, 1.0};
        const std::int64_t day0 = wall() / 86400 * 86400;
        std::string f = "[";
        for (int i = 0; i < 7; ++i)
            f += std::string(i ? "," : "") + "{\"datetime\":" + json::quote(iso_utc(day0 + i * 86400 + 11 * 3600)) +
                 ",\"condition\":\"" + conds[i] + "\",\"temperature\":" + json::number(highs[i]) +
                 ",\"templow\":" + json::number(lows[i]) + ",\"precipitation\":0.0}";
        out("{\"id\":" + sid + ",\"type\":\"event\",\"event\":{\"type\":\"daily\",\"forecast\":" + f + "]}}");
    } else if (type == "history/history_during_period") {
        ++world_.history_reads;
        double t0 = 0, t1 = 0;
        parse_iso(m.get("start_time") ? m.get("start_time")->as_string() : "", t0);
        parse_iso(m.get("end_time") ? m.get("end_time")->as_string() : "", t1);
        std::string r = "{";
        bool first = true;
        if (const json::Value *ids = m.get("entity_ids"))
            for (const json::Value &v : ids->items) {
                const SimEntity *e = world_.find(v.as_string());
                if (!e) continue;
                const double base = std::atof(e->state.c_str());
                std::string pts = "[";
                int n = 0;
                for (double t = t0; t <= t1 && t1 > t0; t += 900, ++n) {
                    const double x = (t - t0) / (t1 - t0);
                    const double val = base + (base > 50 ? 30.0 : 2.5) * std::sin(x * 6.2831853 * 1.5) - (base > 50 ? 10.0 : 1.0) * x;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%.2f", val);
                    pts += std::string(n ? "," : "") + "{\"s\":\"" + buf + "\",\"lu\":" + json::number(t) + "}";
                }
                r += std::string(first ? "" : ",") + json::quote(e->id) + ":" + pts + "]";
                first = false;
            }
        result(r + "}");
    } else if (type == "call_service") {
        const std::string domain = m.get("domain") ? m.get("domain")->as_string() : "";
        const std::string service = m.get("service") ? m.get("service")->as_string() : "";
        std::vector<std::string> targets;
        if (const json::Value *t = m.get("target"))
            if (const json::Value *e = t->get("entity_id")) {
                if (e->is_array())
                    for (const json::Value &x : e->items) targets.push_back(x.as_string());
                else targets.push_back(e->as_string());
            }
        int pct = -1;
        if (const json::Value *d = m.get("service_data"))
            if (const json::Value *b = d->get("brightness_pct")) pct = static_cast<int>(b->as_int(-1));
        std::string all;
        for (const std::string &t : targets) all += (all.empty() ? "" : ",") + t;
        world_.calls.push_back(domain + "." + service + " " + all + (pct >= 0 ? " " + std::to_string(pct) : ""));
        std::vector<SimEntity *> es;
        for (const std::string &t : targets)
            if (SimEntity *e = world_.find(t)) es.push_back(e);
        if (es.empty() || es.size() != targets.size()) {
            out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":"
                             "\"not_found\",\"message\":\"Entity not found\"}}");
            return;
        }
        for (SimEntity *e : es)
            if (e->fail_service == service) {
                out("{\"id\":" + sid + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":"
                                 "\"home_assistant_error\",\"message\":\"Lock jammed\"}}");
                return;
            }
        std::vector<std::pair<SimEntity *, bool>> changes;
        for (SimEntity *e : es) {
            bool removed_brightness = false;
            if (service == "turn_on" && (domain == "light" || domain == "switch" || domain == "input_boolean" || domain == "fan")) {
                e->state = "on";
                if (domain == "light") e->brightness = pct > 0 ? (pct * 255 + 50) / 100 : (e->brightness > 0 ? e->brightness : 255);
            } else if (service == "turn_off") {
                removed_brightness = domain == "light" && e->state == "on";
                e->state = "off";
            } else if (service == "toggle") {
                e->state = e->state == "on" ? "off" : "on";
            } else if (service == "lock") {
                e->state = "locked";
            } else if (service == "unlock") {
                e->state = "unlocked";
            } else if (service == "open_cover") {
                e->state = "open";
            } else if (service == "close_cover") {
                e->state = "closed";
            } else if (domain == "scene") {
                e->state = "2026-11-01T20:00:00+00:00";
            }
            changes.emplace_back(e, removed_brightness);
        }
        result("{\"context\":{\"id\":\"01SIMCTX\"},\"response\":null}");
        if (domain != "scene" && domain != "script")
            for (const auto &c : changes) push_state_change(*c.first, c.second);
    } else {
        out("{\"id\":" + sid +
                         ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"unknown_command\"}}");
    }
}

void SimBackend::phone(const std::string &how)
{
    if (!listening_) return;
    std::map<std::string, std::string> q;
    q["state"] = kSimState;
    if (how == "refused") q["error"] = "access_denied";
    else q["code"] = how == "badcode" ? "wrong" : "sim-code";
    engine_.on_callback("/callback", q, now_);
    run_actions();
}

void SimBackend::drop_link()
{
    if (!link_open_) return;
    link_open_ = authed_ = false;
    sub_id_ = 0;
    sub_ids_.clear();
    inbox_.clear();
    engine_.on_ws_closed("connection reset by peer", now_);
    run_actions();
}

void SimBackend::server_set_state(const std::string &id, const std::string &state)
{
    SimEntity *e = world_.find(id);
    if (!e) return;
    const bool removed = e->state == "on" && state != "on" && id.rfind("light.", 0) == 0;
    e->state = state;
    push_state_change(*e, removed);
}

std::unique_ptr<SimBackend> make_demo_backend(const std::string &name)
{
    SimWorld w = sim_world(name);
    if (const char *p = std::getenv("HA_DEMO_PHONE")) w.phone = p;
    double speed = 1.0;
    if (const char *s = std::getenv("HA_DEMO_SPEED")) speed = std::atof(s);

    Settings settings;
    if (const char *server = std::getenv("HA_DEMO_SERVER")) /* a scene's own server address (a long host name) */
        if (*server) settings.server = server;
    Credentials creds;
    std::vector<std::string> favs;
    const bool signed_in = name != "connect" && name != "connect-unreachable";
    if (signed_in) {
        creds.server = "http://homeassistant.local:8123";
        creds.client_id = "http://192.168.1.42:41234/";
        creds.refresh_token = kSimRefresh;
    }
    /* the Favourites tab of the dashboard: a dimmable light on (its slider row), a light off, a lock, a sensor, a
     * thermostat, a scene, an unavailable light, a garage door */
    if (name == "home" || name == "reconnecting" || name == "offline" || name == "connecting")
        favs = {"light.living_ceiling", "light.reading_lamp", "lock.front_door", "sensor.living_temperature",
                "climate.thermostat",   "scene.evening",      "light.porch",     "cover.garage_door"};
    if (name == "big")
        favs = {"light.workshop_005", "sensor.workshop_002", "switch.workshop_001", "light.living_ceiling"};
    if (name.rfind("dash", 0) == 0) favs = {"light.living_ceiling", "lock.front_door", "sensor.living_temperature"};
    std::map<std::string, std::string> names;
    if (name == "connecting" || name == "dash-offline") /* the names cached by an earlier session: a start offline shows them */
        for (const std::string &id : favs)
            if (const SimEntity *e = w.find(id)) names[id] = e->name;
    /* the layout cached by an earlier session (layout.json): the tabs and tiles, greyed, before any connection */
    std::vector<Area> areas;
    std::vector<RegEntry> entries;
    const std::string cached_dash = name == "dash-offline" ? sim_cached_dashboard(w) : std::string();
    if (name == "connecting" || name == "dash-offline") {
        for (const auto &a : w.areas) areas.push_back(Area{a.first, a.second});
        for (const SimEntity &e : w.entities) {
            if (e.hidden || e.category || !supported(domain_of(e.id))) continue;
            RegEntry r;
            r.id = e.id;
            r.name = e.name;
            r.area = !e.area.empty() ? e.area : (w.device_area.count(e.device) ? w.device_area.at(e.device) : "");
            r.precision = e.precision;
            r.icon = e.icon;
            r.platform = e.platform;
            r.translation_key = e.translation_key;
            entries.push_back(std::move(r));
        }
    }
    auto b = std::make_unique<SimBackend>(std::move(w), settings, creds, favs, speed, true, std::move(names));
    if (!entries.empty()) b->engine().set_cached_layout(std::move(areas), std::move(entries), IconRules());
    if (!cached_dash.empty()) b->engine().set_cached_dashboard(cached_dash);
    if (name == "reconnecting") b->drop_after(1000);
    if (name == "offline") b->drop_after(1000, 20.0);
    return b;
}

} // namespace ha
