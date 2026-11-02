/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: a simulated Home Assistant in memory, and a Backend that runs the real engine against it
 * (no LVGL, no network). The unit tests drive whole flows with it (tests/test_engine.cpp); the render harness shows
 * every page with it (HA_DEMO=<world>, tests/scenes). It speaks the subset the engine uses: /auth/providers,
 * /auth/token (authorization_code, refresh_token), /auth/revoke, and over the WebSocket auth, supported_features,
 * get_config, auth/current_user, the area, device and entity-for-display lists, subscribe_entities (a snapshot of the
 * ids asked, then "c" changes), unsubscribe_events, call_service (light, switch, input_boolean, scene, script, lock,
 * cover, fan), frontend/get_icons (a few made-up rules of the test fixture's own, in the shape Home Assistant serves),
 * ping, and for 0.3.0's stored dashboard lovelace/dashboards/list, lovelace/config, frontend/get_system_data,
 * weather/subscribe_forecast (daily, one event) and history/history_during_period (a made-up curve). It is a stand-in
 * for the tests, not a description of Home Assistant beyond that subset.
 *
 * Worlds (HA_DEMO): connect (no sign-in yet), connect-unreachable, home (signed in, five rooms, eight favourites),
 * empty (signed in, nothing configured), big (a room of 300 entities with long names), connecting (signed in, server
 * down from the start, the layout of an earlier session cached), reconnecting (connects, then the link drops for
 * good), offline (the same, time running 20 times faster so that the line reaches Offline quickly); 0.3.0: dash (the
 * owner-shaped stored dashboard: one masonry view of 12 cards, 9 entities, 2 sensor, 1 weather-forecast, 25 entity
 * references, invented names; a second dashboard of one map card in a panel view), dash-offline (the same cached, the
 * server down), dash-mixed (unsupported cards and rows, an icon-only view, a panel map view, a subview, a hidden view,
 * a second dashboard), dash-long (long names everywhere, six views). HA_DEMO_PHONE=ok|refused|badcode makes the simulated phone
 * answer the QR code one second after it shows. HA_DEMO_SPEED=N runs the simulated time N times faster.
 */
#pragma once

#include "engine.hpp"
#include "view.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ha {

struct SimEntity {
    std::string id, name, area, device, state, unit, device_class;
    int brightness = -1;
    bool dimmable = false;
    bool hidden = false, category = false;
    int precision = -1;
    double current_temperature = -1000, target_temperature = -1000;
    int position = -1;
    std::string fail_service; /* this service answers an error (a jammed lock) */
    std::string icon;         /* the registry's user icon ("ic") */
    std::string attr_icon;    /* the state's "icon" attribute */
    std::string platform = "demo", translation_key;
    std::string temperature_unit; /* a weather entity's */
};

struct SimWorld {
    bool reachable = true;       /* HTTP and WebSocket answer */
    bool refuse_refresh = false; /* /auth/token refresh_token answers 400 */
    bool reject_access = false;  /* the next WebSocket auth answers auth_invalid */
    std::string phone;           /* "", ok, refused, badcode */
    std::string user = "deck";
    std::string ha_version = "2026.10.4";
    std::vector<std::pair<std::string, std::string>> areas;   /* id, name */
    std::map<std::string, std::string> device_area;           /* device id -> area */
    std::vector<SimEntity> entities;
    bool icons = true; /* frontend/get_icons answers (false: an older server, "unknown_command") */
    /* the stored dashboards (0.3.0) */
    std::string default_config;  /* lovelace/config without url_path: its JSON ("" config_not_found) */
    std::vector<std::pair<std::string, std::string>> dashboards; /* listed: url_path, title */
    std::map<std::string, std::string> dashboard_configs;        /* url_path -> its JSON ("" config_not_found) */
    std::string default_panel;   /* frontend/get_system_data core.default_panel ("" not set) */
    bool lovelace = true;        /* false: the lovelace commands are unknown */
    bool forecasts = true;       /* weather/subscribe_forecast answers */

    /* counters for the tests */
    int refreshes = 0, revokes = 0, exchanges = 0, ws_opens = 0, auths = 0, provider_checks = 0;
    int config_reads = 0, forecast_subscribes = 0, history_reads = 0;
    std::vector<std::string> calls; /* "light.turn_on light.x {..}" */
    std::vector<std::string> sent;  /* every message the engine sent (tokens included: tests only) */

    SimEntity *find(const std::string &id);
};

/* The worlds of the render harness and the tests. */
SimWorld sim_world(const std::string &name);
/* dashboard.json's text for a world's default dashboard, as an earlier session would have saved it */
std::string sim_cached_dashboard(const SimWorld &w);

/* Runs an Engine against a SimWorld: executes the engine's actions, delivers the simulated server's answers one poll
 * later (a message round trip), keeps the "files" in memory. Time: a virtual clock moved by poll() (real time times
 * speed) or by advance() in the tests. */
class SimBackend : public Backend {
public:
    SimBackend(SimWorld world, Settings settings, Credentials creds, std::vector<std::string> favourites,
               double speed = 1.0, bool real_time = true, std::map<std::string, std::string> names = {});

    std::uint64_t version() override { return engine_.version() * 1000003u + extra_version_; }
    View snapshot() override;
    void send(const Command &c) override;
    void poll() override;

    /* tests: run the world for ms of virtual time in steps */
    void advance(std::int64_t ms, std::int64_t step = 50);
    /* the simulated phone scans the code: "ok", "refused" (error=access_denied) or "badcode" */
    void phone(const std::string &how);
    /* the link drops (a TCP reset); reachable stays as it is */
    void drop_link();
    /* a state changes on the server side (a person at the wall switch) */
    void server_set_state(const std::string &id, const std::string &state);
    /* after ms of virtual time the server goes away for good and the link drops (the reconnecting worlds) */
    /* real_ms after the world is connected and loaded, the server goes away for good and the link drops (the
     * reconnecting worlds); then the virtual time runs `speed_after` times faster (the offline world) */
    void drop_after(std::int64_t real_ms, double speed_after = 1.0)
    {
        drop_real_ms_ = real_ms;
        speed_after_ = speed_after;
    }

    Engine &engine() { return engine_; }
    SimWorld &world() { return world_; }
    std::int64_t now() const { return now_; }
    const Credentials &saved_credentials() const { return saved_creds_; }
    bool credentials_file() const { return creds_file_; }
    const std::vector<std::string> &saved_favourites() const { return saved_favs_; }
    const std::map<std::string, std::string> &saved_names() const { return saved_names_; }
    const std::string &saved_layout() const { return saved_layout_; }
    int layout_saves() const { return layout_saves_; }
    const std::string &saved_dashboard() const { return saved_dashboard_; }
    int dashboard_saves() const { return dashboard_saves_; }
    bool listening() const { return listening_; }

private:
    void run_actions();
    void exec(const Action &a);
    void ws_receive(const std::string &text);
    void deliver();
    void push_state_change(const SimEntity &e, bool removed_brightness);
    std::string compressed(const SimEntity &e) const;
    std::int64_t wall() const;

    SimWorld world_;
    Engine engine_;
    double speed_;
    bool real_time_;
    std::int64_t now_ = 1000000;
    std::int64_t real_last_ = 0;
    std::int64_t demo_started_ = 0;
    bool link_open_ = false, authed_ = false, listening_ = false;
    int sub_id_ = 0;
    std::set<std::string> sub_ids_;
    std::deque<std::string> inbox_; /* server -> engine, delivered at the next poll ("<link serial>|<text>") */
    int link_serial_ = 0;           /* a message of an older link is never delivered to a newer one */
    void out(const std::string &text) { inbox_.push_back(std::to_string(link_serial_) + "|" + text); }
    Credentials saved_creds_;
    bool creds_file_ = false;
    std::vector<std::string> saved_favs_;
    std::map<std::string, std::string> saved_names_;
    std::string saved_layout_;
    int layout_saves_ = 0;
    std::string saved_dashboard_;
    int dashboard_saves_ = 0;
    std::string issued_refresh_;
    int token_serial_ = 0;
    std::string demo_;
    std::int64_t qr_seen_at_ = 0;
    bool phone_done_ = false;
    std::int64_t drop_at_ = 0;       /* virtual time of the drop, once the world is loaded */
    std::int64_t drop_real_ms_ = 0;  /* real ms after loading */
    double speed_after_ = 1.0;
    std::uint64_t extra_version_ = 0;
};

/* The backend for HA_DEMO (render harness): the named world with its starting files. */
std::unique_ptr<SimBackend> make_demo_backend(const std::string &name);

} // namespace ha
