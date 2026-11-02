/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: the client state machine, driven by scripted events (no network). The JSON the engine sends is checked
 * byte for byte where Home Assistant's API fixes it.
 */
#include "test_util.hpp"

#include "engine.hpp"
#include "json.hpp"

#include <algorithm>

using namespace ha;

namespace {

constexpr std::int64_t T0 = 1000000;
constexpr std::int64_t WALL = 1762000000;
const char *kState = "0123456789abcdef0123456789abcdef";

std::vector<Action> of_kind(const std::vector<Action> &acts, Action::Kind k)
{
    std::vector<Action> out;
    for (const Action &a : acts)
        if (a.kind == k) out.push_back(a);
    return out;
}

bool has(const std::vector<Action> &acts, Action::Kind k)
{
    return !of_kind(acts, k).empty();
}

std::vector<std::string> sent(const std::vector<Action> &acts)
{
    std::vector<std::string> out;
    for (const Action &a : of_kind(acts, Action::Kind::WsSend)) out.push_back(a.body);
    return out;
}

Credentials creds()
{
    Credentials c;
    c.server = "http://homeassistant.local:8123";
    c.client_id = "http://192.168.1.42:41234/";
    c.refresh_token = "REFRESH-SECRET";
    return c;
}

const char *kTokenOk = "{\"access_token\":\"ACCESS-1\",\"token_type\":\"Bearer\",\"expires_in\":1800}";

/* The connection thread ticks the engine at least once a second; a longer gap means the process was stopped. Ticks
 * from..to (inclusive of to) every second; with alive, a message arrives before each tick (the link is busy). Returns
 * every action taken on the way. */
std::vector<Action> ticks(Engine &e, std::int64_t from, std::int64_t to, bool alive)
{
    std::vector<Action> all;
    for (std::int64_t t = from; ; t += 1000) {
        if (t > to) t = to;
        if (alive) e.on_ws_message("{\"id\":0,\"type\":\"pong\"}", t, WALL);
        e.on_tick(t, WALL);
        for (Action &a : e.take_actions()) all.push_back(std::move(a));
        if (t == to) break;
    }
    return all;
}

/* An engine signed in and online with a small registry; returns the time it reached. */
std::int64_t bring_online(Engine &e, std::vector<std::string> *out_sent = nullptr)
{
    std::int64_t t = T0;
    e.start(t);
    auto a = e.take_actions();
    e.on_http(HttpTag::Refresh, 200, kTokenOk, "", t);
    a = e.take_actions();
    e.on_ws_open(true, "", t);
    e.on_ws_message("{\"type\":\"auth_required\",\"ha_version\":\"2026.10.4\"}", t, WALL);
    a = e.take_actions();
    e.on_ws_message("{\"type\":\"auth_ok\",\"ha_version\":\"2026.10.4\"}", t, WALL);
    a = e.take_actions();
    if (out_sent) *out_sent = sent(a);
    e.on_ws_message("[{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":null},"
                    "{\"id\":2,\"type\":\"result\",\"success\":true,\"result\":{\"unit_system\":{\"temperature\":\"\\u00b0C\"}}},"
                    "{\"id\":3,\"type\":\"result\",\"success\":true,\"result\":{\"name\":\"deck\",\"is_admin\":false}}]",
                    t, WALL);
    e.on_ws_message("{\"id\":4,\"type\":\"result\",\"success\":true,\"result\":[{\"area_id\":\"kitchen\",\"name\":\"Kitchen\"}]}", t,
                    WALL);
    e.on_ws_message("{\"id\":5,\"type\":\"result\",\"success\":true,\"result\":[]}", t, WALL);
    e.on_ws_message("{\"id\":6,\"type\":\"result\",\"success\":true,\"result\":{\"entities\":["
                    "{\"ei\":\"light.k\",\"en\":\"Kitchen light\",\"ai\":\"kitchen\"},"
                    "{\"ei\":\"switch.coffee\",\"en\":\"Coffee\",\"ai\":\"kitchen\"},"
                    "{\"ei\":\"scene.evening\",\"en\":\"Evening\"},"
                    "{\"ei\":\"lock.front\",\"en\":\"Front door\"}]}}",
                    t, WALL);
    return t;
}

} // namespace

static void test_pairing()
{
    Engine e(Settings(), Credentials(), {});
    e.start(T0);
    auto a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpGet);
    CHECK_EQ(a[0].url, std::string("http://homeassistant.local:8123/auth/providers"));
    CHECK(e.view(T0, WALL).conn == Conn::SignedOut);
    CHECK(!e.view(T0, WALL).signed_in);

    e.on_http(HttpTag::CheckServer, 200, "{\"providers\":[]}", "", T0);
    a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::ListenStart);
    CHECK_EQ(a[0].host, std::string("homeassistant.local"));
    e.on_listen(true, "192.168.1.42", 41234, "192.168.1.10", kState, "", T0);
    View v = e.view(T0, WALL);
    CHECK_EQ(v.qr_url, std::string("http://homeassistant.local:8123/auth/authorize?client_id=http%3A%2F%2F192.168.1.42%3A41234%2F"
                                   "&redirect_uri=http%3A%2F%2F192.168.1.42%3A41234%2Fcallback&state=") + kState);
    CHECK_EQ(v.pair_status, std::string("Waiting for the phone..."));
    CHECK(e.listening());

    /* not this code: wrong state, the deck's own page, no code */
    e.on_callback("/callback", {{"code", "abc"}, {"state", "wrong-state-wrong-state-wrong-st"}}, T0);
    e.on_callback("/", {}, T0);
    e.on_callback("/callback", {{"state", kState}}, T0);
    CHECK(e.take_actions().empty());
    /* login refused on the phone */
    e.on_callback("/callback", {{"error", "access_denied"}, {"state", kState}}, T0);
    CHECK(e.view(T0, WALL).pair_tone == Tone::Error);
    CHECK(e.view(T0, WALL).pair_status.find("refused") != std::string::npos);
    CHECK(e.take_actions().empty());
    /* the code */
    e.on_callback("/callback", {{"code", "CODE-1"}, {"state", kState}}, T0 + 10);
    a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpPost && a[0].tag == HttpTag::Exchange);
    CHECK_EQ(a[0].url, std::string("http://homeassistant.local:8123/auth/token"));
    CHECK_EQ(a[0].body, std::string("grant_type=authorization_code&code=CODE-1&client_id=http%3A%2F%2F192.168.1.42%3A41234%2F"));
    /* refused code: back to waiting, the listener stays */
    e.on_http(HttpTag::Exchange, 400, "{\"error\":\"invalid_request\",\"error_description\":\"Invalid code\"}", "", T0 + 20);
    CHECK(e.listening());
    CHECK(e.view(T0, WALL).pair_status.find("Invalid code") != std::string::npos);
    CHECK(e.take_actions().empty());
    e.on_callback("/callback", {{"code", "CODE-2"}, {"state", kState}}, T0 + 30);
    a = e.take_actions();
    e.on_http(HttpTag::Exchange, 200,
              "{\"access_token\":\"ACCESS-0\",\"token_type\":\"Bearer\",\"refresh_token\":\"REFRESH-NEW\",\"expires_in\":1800}",
              "", T0 + 40);
    a = e.take_actions();
    CHECK(has(a, Action::Kind::SaveCredentials));
    CHECK(has(a, Action::Kind::ListenStop));
    CHECK(has(a, Action::Kind::WsOpen));
    CHECK(!has(a, Action::Kind::HttpPost)); /* a fresh token: no refresh before connecting */
    CHECK_EQ(of_kind(a, Action::Kind::WsOpen)[0].url, std::string("ws://homeassistant.local:8123/api/websocket"));
    CHECK_EQ(e.credentials().client_id, std::string("http://192.168.1.42:41234/"));
    CHECK_EQ(e.credentials().refresh_token, std::string("REFRESH-NEW"));
    CHECK_EQ(e.credentials().server, std::string("http://homeassistant.local:8123"));
    CHECK(e.signed_in());
    e.on_ws_open(true, "", T0 + 50);
    e.on_ws_message("{\"type\":\"auth_required\"}", T0 + 60, WALL);
    a = e.take_actions();
    CHECK_EQ(sent(a)[0], std::string("{\"type\":\"auth\",\"access_token\":\"ACCESS-0\"}"));
    e.on_ws_message("{\"type\":\"auth_ok\",\"ha_version\":\"2026.10.4\"}", T0 + 70, WALL);
    e.take_actions();
    e.on_ws_message("{\"id\":3,\"type\":\"result\",\"success\":true,\"result\":{\"name\":\"deck\"}}", T0 + 80, WALL);
    v = e.view(T0 + 80, WALL);
    CHECK(v.paired_now);
    CHECK_EQ(v.pair_status, std::string("Connected as deck"));
    CHECK_EQ(v.user, std::string("deck"));
    CHECK(v.conn == Conn::Connected);
}

static void test_pairing_failures()
{
    /* server unreachable: a failure line, a new check after the back-off */
    Engine e(Settings(), Credentials(), {});
    e.start(T0);
    e.take_actions();
    e.on_http(HttpTag::CheckServer, 0, "", "Could not resolve host", T0);
    View v = e.view(T0, WALL);
    CHECK(v.pair_tone == Tone::Error);
    CHECK_EQ(v.pair_status, std::string("Cannot reach Home Assistant at homeassistant.local:8123. Check the server address."));
    CHECK(v.qr_url.empty());
    e.on_tick(T0 + 1999, WALL);
    CHECK(e.take_actions().empty());
    e.on_tick(T0 + 2000, WALL);
    auto a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpGet);

    /* the QR code names Home Assistant by its IP address when the setting says so */
    Settings s;
    s.qr_use_ip = true;
    Engine e2(s, Credentials(), {});
    e2.start(T0);
    e2.take_actions();
    e2.on_http(HttpTag::CheckServer, 200, "", "", T0);
    e2.take_actions();
    e2.on_listen(true, "192.168.1.42", 41234, "192.168.1.10", kState, "", T0);
    CHECK(e2.view(T0, WALL).qr_url.rfind("http://192.168.1.10:8123/auth/authorize?", 0) == 0);

    /* a public deck address is refused (Home Assistant would refuse the client id) */
    Engine e3(Settings(), Credentials(), {});
    e3.start(T0);
    e3.take_actions();
    e3.on_http(HttpTag::CheckServer, 200, "", "", T0);
    e3.take_actions();
    e3.on_listen(true, "8.8.8.8", 41234, "192.168.1.10", kState, "", T0);
    CHECK(e3.view(T0, WALL).qr_url.empty());
    CHECK(e3.view(T0, WALL).pair_tone == Tone::Error);

    /* nothing back from the phone for 3 minutes: the hint */
    Engine e4(Settings(), Credentials(), {});
    e4.start(T0);
    e4.take_actions();
    e4.on_http(HttpTag::CheckServer, 200, "", "", T0);
    e4.take_actions();
    e4.on_listen(true, "192.168.1.42", 41234, "", kState, "", T0);
    e4.on_tick(T0 + 181000, WALL);
    CHECK(e4.view(T0, WALL).pair_status.find("IP address") != std::string::npos);
    CHECK(e4.view(T0, WALL).pair_tone == Tone::Warn);

    /* an invalid server address */
    Settings bad;
    bad.server = "";
    Engine e5(bad, Credentials(), {});
    e5.start(T0);
    CHECK(e5.take_actions().empty());
    CHECK(e5.view(T0, WALL).pair_status.find("server address") != std::string::npos);
    e5.on_command(Command::of(Command::Kind::SetServer, "ha.lan:8123"), T0);
    auto a5 = e5.take_actions();
    CHECK(has(a5, Action::Kind::SaveSettings));
    CHECK(has(a5, Action::Kind::HttpGet));
    CHECK_EQ(e5.settings().server, std::string("ha.lan:8123"));
    e5.on_command(Command::of(Command::Kind::SetServer, "bad host"), T0);
    CHECK(e5.view(T0, WALL).message_error);
}

static void test_connect_and_messages()
{
    Engine e(Settings(), creds(), {"light.k", "scene.evening"});
    e.start(T0);
    auto a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpPost && a[0].tag == HttpTag::Refresh);
    CHECK_EQ(a[0].url, std::string("http://homeassistant.local:8123/auth/token"));
    CHECK_EQ(a[0].body, std::string("grant_type=refresh_token&refresh_token=REFRESH-SECRET&client_id=http%3A%2F%2F192.168.1.42%3A41234%2F"));
    CHECK(e.view(T0, WALL).conn == Conn::Connecting);

    Engine e2(Settings(), creds(), {"light.k", "scene.evening"});
    std::vector<std::string> first;
    const std::int64_t t = bring_online(e2, &first);
    CHECK(first.size() == 6);
    CHECK_EQ(first[0], std::string("{\"id\":1,\"type\":\"supported_features\",\"features\":{\"coalesce_messages\":1}}"));
    CHECK_EQ(first[1], std::string("{\"id\":2,\"type\":\"get_config\"}"));
    CHECK_EQ(first[2], std::string("{\"id\":3,\"type\":\"auth/current_user\"}"));
    CHECK_EQ(first[3], std::string("{\"id\":4,\"type\":\"config/area_registry/list\"}"));
    CHECK_EQ(first[4], std::string("{\"id\":5,\"type\":\"config/device_registry/list\"}"));
    CHECK_EQ(first[5], std::string("{\"id\":6,\"type\":\"config/entity_registry/list_for_display\"}"));
    auto a2 = e2.take_actions();
    auto s2 = sent(a2);
    CHECK(s2.size() == 5);
    CHECK_EQ(s2[0], std::string("{\"id\":7,\"type\":\"subscribe_entities\",\"entity_ids\":[\"light.k\",\"scene.evening\"]}"));
    /* 0.2.0: the domains' icon rules (no entity of this registry has a translation key: no "entity" request) */
    CHECK_EQ(s2[1], std::string("{\"id\":8,\"type\":\"frontend/get_icons\",\"category\":\"entity_component\",\"integration\":"
                                "[\"binary_sensor\",\"climate\",\"cover\",\"input_boolean\",\"light\",\"lock\",\"scene\","
                                "\"script\",\"sensor\",\"switch\"]}"));
    /* 0.3.0: the stored dashboards (any user may read them), the system's default panel, the default dashboard */
    CHECK_EQ(s2[2], std::string("{\"id\":9,\"type\":\"lovelace/dashboards/list\"}"));
    CHECK_EQ(s2[3], std::string("{\"id\":10,\"type\":\"frontend/get_system_data\",\"key\":\"core\"}"));
    CHECK_EQ(s2[4], std::string("{\"id\":11,\"type\":\"lovelace/config\"}"));
    CHECK(has(a2, Action::Kind::SaveLayout));
    View v = e2.view(t, WALL);
    CHECK(v.conn == Conn::Connected);
    CHECK_EQ(v.user, std::string("deck"));
    CHECK_EQ(v.ha_version, std::string("2026.10.4"));
    CHECK(v.favourites.size() == 2);
    CHECK(v.rooms.size() == 2); /* Kitchen, then No room (the scene and the lock) */
    CHECK_EQ(v.rooms[0].name, std::string("Kitchen"));
    CHECK(v.rooms[0].count == 2);
    CHECK_EQ(v.rooms[1].name, std::string("No room"));

    /* the snapshot, then a change, in a coalesced array; an old subscription's event ignored */
    e2.on_ws_message("{\"id\":7,\"type\":\"event\",\"event\":{\"a\":{\"light.k\":{\"s\":\"off\",\"a\":{\"friendly_name\":\"Kitchen light\","
                     "\"supported_color_modes\":[\"brightness\"]},\"c\":\"x\",\"lc\":1762000000},"
                     "\"scene.evening\":{\"s\":\"2026-10-31T19:00:00+00:00\",\"a\":{\"friendly_name\":\"Evening\"},\"c\":\"y\",\"lc\":1}}}}",
                     t, WALL);
    v = e2.view(t, WALL);
    CHECK_EQ(v.favourites[0].value, std::string("Off"));
    CHECK(!v.favourites[0].muted);
    CHECK(v.last_update == WALL);
    e2.on_ws_message("[{\"id\":7,\"type\":\"event\",\"event\":{\"c\":{\"light.k\":{\"+\":{\"s\":\"on\",\"a\":{\"brightness\":255}}}}}},"
                     "{\"id\":99,\"type\":\"event\",\"event\":{\"c\":{\"light.k\":{\"+\":{\"s\":\"off\"}}}}}]",
                     t, WALL);
    v = e2.view(t, WALL);
    CHECK_EQ(v.favourites[0].value, std::string("On"));

    /* commands: the exact call_service messages */
    e2.on_command(Command::of(Command::Kind::Toggle, "light.k"), t);
    auto c = sent(e2.take_actions());
    CHECK_EQ(c[0], std::string("{\"id\":12,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_off\","
                               "\"target\":{\"entity_id\":\"light.k\"}}"));
    v = e2.view(t, WALL);
    CHECK(v.favourites[0].pending);
    CHECK_EQ(v.favourites[0].value, std::string("..."));
    e2.on_ws_message("{\"id\":12,\"type\":\"result\",\"success\":true,\"result\":{\"context\":{}}}", t, WALL);
    CHECK(e2.view(t, WALL).favourites[0].pending); /* still waiting for the new state */
    e2.on_ws_message("{\"id\":7,\"type\":\"event\",\"event\":{\"c\":{\"light.k\":{\"+\":{\"s\":\"off\"},\"-\":{\"a\":[\"brightness\"]}}}}}",
                     t, WALL);
    v = e2.view(t, WALL);
    CHECK(!v.favourites[0].pending);
    CHECK_EQ(v.favourites[0].value, std::string("Off"));

    e2.on_command(Command::of(Command::Kind::Toggle, "switch.coffee"), t);
    CHECK_EQ(sent(e2.take_actions())[0], std::string("{\"id\":13,\"type\":\"call_service\",\"domain\":\"switch\",\"service\":\"toggle\","
                                                     "\"target\":{\"entity_id\":\"switch.coffee\"}}"));
    e2.on_command(Command::of(Command::Kind::Run, "scene.evening"), t);
    CHECK_EQ(sent(e2.take_actions())[0], std::string("{\"id\":14,\"type\":\"call_service\",\"domain\":\"scene\",\"service\":\"turn_on\","
                                                     "\"target\":{\"entity_id\":\"scene.evening\"}}"));
    e2.on_ws_message("{\"id\":14,\"type\":\"result\",\"success\":true,\"result\":null}", t, WALL);
    CHECK(e2.view(t + 100, WALL).favourites[1].done);
    CHECK_EQ(e2.view(t + 100, WALL).favourites[1].value, std::string("Done"));
    e2.on_tick(t + 1600, WALL);
    CHECK(!e2.view(t + 1600, WALL).favourites[1].done);
    e2.on_command(Command::of(Command::Kind::SetBrightness, "light.k", "", 50), t);
    CHECK_EQ(sent(e2.take_actions())[0], std::string("{\"id\":15,\"type\":\"call_service\",\"domain\":\"light\",\"service\":\"turn_on\","
                                                     "\"service_data\":{\"brightness_pct\":50},\"target\":{\"entity_id\":\"light.k\"}}"));
    e2.on_command(Command::of(Command::Kind::SetBrightness, "light.k", "", 0), t);
    CHECK(sent(e2.take_actions())[0].find("\"service\":\"turn_off\"") != std::string::npos);
    e2.on_command(Command::of(Command::Kind::Service, "lock.front", "unlock"), t);
    CHECK_EQ(sent(e2.take_actions())[0], std::string("{\"id\":17,\"type\":\"call_service\",\"domain\":\"lock\",\"service\":\"unlock\","
                                                     "\"target\":{\"entity_id\":\"lock.front\"}}"));
    e2.on_command(Command::of(Command::Kind::Service, "lock.front", "open_cover"), t); /* not a lock service: nothing */
    e2.on_command(Command::of(Command::Kind::Run, "light.k"), t);                       /* not a scene: nothing */
    CHECK(sent(e2.take_actions()).empty());
    /* a failed call: the message names the entity and Home Assistant's reason; the "..." goes */
    e2.on_ws_message("{\"id\":17,\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"home_assistant_error\",\"message\":\"Lock jammed\"}}",
                     t, WALL);
    v = e2.view(t, WALL);
    CHECK(v.message_error);
    CHECK_EQ(v.message, std::string("Could not unlock Front door: Lock jammed"));
    /* "..." lasts 5 s at most */
    e2.on_command(Command::of(Command::Kind::Toggle, "switch.coffee"), t);
    e2.take_actions();
    e2.on_tick(t + 5001, WALL);
    for (const EntityRow &r : e2.view(t + 5001, WALL).favourites) CHECK(!r.pending);

    /* the room shown: unsubscribe the old set, subscribe the union */
    e2.on_command(Command::set_view(true, "kitchen", ""), t);
    auto rs = sent(e2.take_actions());
    CHECK(rs.size() == 2);
    CHECK_EQ(rs[0], std::string("{\"id\":19,\"type\":\"unsubscribe_events\",\"subscription\":7}"));
    CHECK_EQ(rs[1], std::string("{\"id\":20,\"type\":\"subscribe_entities\",\"entity_ids\":[\"light.k\",\"scene.evening\",\"switch.coffee\"]}"));
    v = e2.view(t, WALL);
    CHECK(v.room_shown && v.room.size() == 2);
    CHECK_EQ(v.room_name, std::string("Kitchen"));
    /* the old subscription's events no longer apply */
    e2.on_ws_message("{\"id\":7,\"type\":\"event\",\"event\":{\"c\":{\"light.k\":{\"+\":{\"s\":\"on\"}}}}}", t, WALL);
    CHECK_EQ(e2.model().state("light.k")->state, std::string("off"));
    /* the same view again: nothing sent */
    e2.on_command(Command::set_view(true, "kitchen", ""), t);
    CHECK(sent(e2.take_actions()).empty());
    /* an entity page outside the set */
    e2.on_command(Command::set_view(false, "", "lock.front"), t);
    rs = sent(e2.take_actions());
    CHECK(rs.size() == 2 && rs[1].find("\"lock.front\"") != std::string::npos);
    v = e2.view(t, WALL);
    CHECK_EQ(v.detail.id, std::string("lock.front"));
    CHECK(v.detail.confirm);
    /* favourites: starred and saved, subscription follows */
    e2.on_command(Command::of(Command::Kind::ToggleFavourite, "lock.front"), t);
    auto fa = e2.take_actions();
    CHECK(has(fa, Action::Kind::SaveFavourites));
    CHECK(e2.favourites().size() == 3 && e2.favourites()[2] == "lock.front");
    CHECK(e2.view(t, WALL).detail.favourite);
    e2.on_command(Command::of(Command::Kind::ToggleFavourite, "light.k"), t);
    e2.take_actions();
    CHECK(e2.favourites().size() == 2 && e2.favourites()[0] == "scene.evening");
}

static void test_backoff_and_states()
{
    CHECK(backoff_ms(1) == 2000);
    CHECK(backoff_ms(2) == 4000);
    CHECK(backoff_ms(3) == 8000);
    CHECK(backoff_ms(4) == 16000);
    CHECK(backoff_ms(5) == 32000);
    CHECK(backoff_ms(6) == 60000);
    CHECK(backoff_ms(20) == 60000);

    Engine e(Settings(), creds(), {"light.k"});
    std::int64_t t = bring_online(e);
    e.take_actions();
    CHECK(e.view(t, WALL).conn == Conn::Connected);
    /* the link drops: Reconnecting, a retry after 2 s with the token still valid (no refresh) */
    e.on_ws_closed("connection reset by peer", t);
    auto a = e.take_actions();
    CHECK(has(a, Action::Kind::WsClose));
    View v = e.view(t, WALL);
    CHECK(v.conn == Conn::Reconnecting);
    CHECK(v.favourites[0].muted); /* last value greyed */
    e.on_tick(t + 1999, WALL);
    CHECK(e.take_actions().empty());
    e.on_tick(t + 2000, WALL);
    a = e.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::WsOpen);
    /* two more failures: 4 s, then 8 s; the third makes the line say Offline */
    e.on_ws_open(false, "Connection refused", t + 2000);
    e.take_actions();
    CHECK(e.view(t, WALL).conn == Conn::Reconnecting);
    e.on_tick(t + 2000 + 3999, WALL);
    CHECK(e.take_actions().empty());
    e.on_tick(t + 6000, WALL);
    CHECK(has(e.take_actions(), Action::Kind::WsOpen));
    e.on_ws_open(false, "Connection refused", t + 6000);
    e.take_actions();
    CHECK(e.view(t, WALL).conn == Conn::Offline);
    e.on_tick(t + 6000 + 8000, WALL);
    CHECK(has(e.take_actions(), Action::Kind::WsOpen));
    /* a connection that answers: back to Connected, the back-off reset */
    e.on_ws_open(true, "", t + 14000);
    e.on_ws_message("{\"type\":\"auth_required\"}", t + 14000, WALL);
    e.on_ws_message("{\"type\":\"auth_ok\"}", t + 14000, WALL);
    CHECK(e.view(t, WALL).conn == Conn::Connected);
    e.take_actions();
    e.on_ws_closed("x", t + 15000);
    e.take_actions();
    e.on_tick(t + 17000, WALL);
    CHECK(has(e.take_actions(), Action::Kind::WsOpen)); /* 2 s again */

    /* never connected and failing: Connecting, then Offline */
    Engine n(Settings(), creds(), {});
    n.start(T0);
    n.take_actions();
    n.on_http(HttpTag::Refresh, 0, "", "Could not resolve host", T0);
    n.take_actions();
    CHECK(n.view(T0, WALL).conn == Conn::Connecting);
    n.on_tick(T0 + 2000, WALL);
    auto na = n.take_actions();
    CHECK(na.size() == 1 && na[0].tag == HttpTag::Refresh); /* no token yet: refresh again */
}

static void test_tokens_and_refusals()
{
    /* the access token is refreshed 2 minutes before it ends, while connected */
    Engine e(Settings(), creds(), {});
    std::int64_t t = bring_online(e);
    e.take_actions();
    auto a = ticks(e, t, t + 1680000 - 1, true);
    CHECK(of_kind(a, Action::Kind::HttpPost).empty());
    CHECK(e.online());
    a = ticks(e, t + 1680001, t + 1680001, true);
    CHECK(of_kind(a, Action::Kind::HttpPost).size() == 1 && of_kind(a, Action::Kind::HttpPost)[0].tag == HttpTag::Refresh);
    a = ticks(e, t + 1680100, t + 1682100, true);
    CHECK(of_kind(a, Action::Kind::HttpPost).empty()); /* one at a time */
    e.on_http(HttpTag::Refresh, 200, "{\"access_token\":\"ACCESS-2\",\"expires_in\":1800}", "", t + 1682200);
    CHECK(e.online());
    /* the next connect uses ACCESS-2 without a refresh */
    e.on_ws_closed("x", t + 1682300);
    e.take_actions();
    a = ticks(e, t + 1683300, t + 1684300, false);
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::WsOpen);
    e.on_ws_open(true, "", t + 1684300);
    e.on_ws_message("{\"type\":\"auth_required\"}", t + 1684300, WALL);
    CHECK_EQ(sent(e.take_actions())[0], std::string("{\"type\":\"auth\",\"access_token\":\"ACCESS-2\"}"));

    /* a short token (40 s): refreshed once, a quarter of its life before the end, never in a loop */
    {
        Engine s(Settings(), creds(), {});
        s.start(T0);
        s.take_actions();
        s.on_http(HttpTag::Refresh, 200, "{\"access_token\":\"SHORT\",\"expires_in\":40}", "", T0);
        s.take_actions();
        s.on_ws_open(true, "", T0);
        s.on_ws_message("{\"type\":\"auth_required\"}", T0, WALL);
        s.on_ws_message("{\"type\":\"auth_ok\"}", T0, WALL);
        s.take_actions();
        auto sa = ticks(s, T0, T0 + 29000, true);
        CHECK(of_kind(sa, Action::Kind::HttpPost).empty());
        sa = ticks(s, T0 + 30001, T0 + 39000, true);
        CHECK(of_kind(sa, Action::Kind::HttpPost).size() == 1);
        s.on_http(HttpTag::Refresh, 200, "{\"access_token\":\"SHORT2\",\"expires_in\":40}", "", T0 + 39000);
        sa = ticks(s, T0 + 40000, T0 + 68000, true);
        CHECK(of_kind(sa, Action::Kind::HttpPost).empty());
    }

    /* a token close to its end is refreshed before connecting */
    Engine f(Settings(), creds(), {});
    t = bring_online(f);
    f.take_actions();
    ticks(f, t, t + 1670000, true);
    f.on_ws_closed("x", t + 1750000); /* 50 s left (the early refresh is answered by nobody here) */
    f.take_actions();
    a = ticks(f, t + 1751000, t + 1752000, false);
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpPost && a[0].tag == HttpTag::Refresh);

    /* auth_invalid with a token older than a minute: one refresh, one more try; then a second refusal stops */
    Engine g(Settings(), creds(), {});
    t = bring_online(g);
    g.take_actions();
    ticks(g, t, t + 120000, true);
    g.on_ws_closed("x", t + 120000);
    g.take_actions();
    a = ticks(g, t + 121000, t + 122000, false);
    CHECK(has(a, Action::Kind::WsOpen));
    g.on_ws_open(true, "", t + 122000);
    g.on_ws_message("{\"type\":\"auth_required\"}", t + 122000, WALL);
    g.take_actions();
    g.on_ws_message("{\"type\":\"auth_invalid\",\"message\":\"Invalid access token or password\"}", t + 122100, WALL);
    a = g.take_actions();
    CHECK(has(a, Action::Kind::WsClose));
    CHECK(of_kind(a, Action::Kind::HttpPost).size() == 1 && of_kind(a, Action::Kind::HttpPost)[0].tag == HttpTag::Refresh);
    CHECK(g.signed_in());
    g.on_http(HttpTag::Refresh, 200, "{\"access_token\":\"ACCESS-3\",\"expires_in\":1800}", "", t + 122200);
    a = g.take_actions();
    CHECK(has(a, Action::Kind::WsOpen));
    g.on_ws_open(true, "", t + 122300);
    g.on_ws_message("{\"type\":\"auth_required\"}", t + 122300, WALL);
    g.take_actions();
    g.on_ws_message("{\"type\":\"auth_invalid\"}", t + 122400, WALL);
    a = g.take_actions();
    CHECK(!g.signed_in());
    CHECK(has(a, Action::Kind::DeleteCredentials));
    auto revoke = of_kind(a, Action::Kind::HttpPost);
    CHECK(revoke.size() == 1 && revoke[0].tag == HttpTag::RevokeQuiet);
    CHECK_EQ(revoke[0].body, std::string("token=REFRESH-SECRET"));
    CHECK(has(a, Action::Kind::HttpGet)); /* back to pairing */
    View v = g.view(t, WALL);
    CHECK(v.conn == Conn::SignedOut);
    CHECK(v.pair_tone == Tone::Error);
    CHECK(v.favourites.empty() || v.rooms.empty());

    /* a refresh refused (revoked in Home Assistant): never retried, signed out, no revoke call */
    Engine h(Settings(), creds(), {"light.k"});
    h.start(T0);
    h.take_actions();
    h.on_http(HttpTag::Refresh, 400, "{\"error\":\"invalid_grant\"}", "", T0);
    a = h.take_actions();
    CHECK(!h.signed_in());
    CHECK(has(a, Action::Kind::DeleteCredentials));
    CHECK(of_kind(a, Action::Kind::HttpPost).empty());
    CHECK(h.view(T0, WALL).pair_status.find("no longer accepts") != std::string::npos);
    CHECK(h.favourites().size() == 1); /* kept for the next sign-in */
}

static void test_ping_freeze_signout()
{
    Engine e(Settings(), creds(), {});
    std::int64_t t = bring_online(e);
    e.take_actions();
    /* 30 s without any message: a ping; 15 s without an answer: the link is dropped */
    CHECK(sent(ticks(e, t, t + 29999, false)).empty());
    auto s = sent(ticks(e, t + 30001, t + 30001, false));
    CHECK(s.size() == 1 && s[0].find("\"type\":\"ping\"") != std::string::npos);
    ticks(e, t + 31001, t + 30001 + 14999, false);
    CHECK(e.online());
    auto dead = ticks(e, t + 30001 + 15001, t + 30001 + 15001, false);
    CHECK(!e.online());
    CHECK(has(dead, Action::Kind::WsClose));
    /* the answer keeps the link */
    Engine e2(Settings(), creds(), {});
    t = bring_online(e2);
    e2.take_actions();
    const std::string ping = sent(ticks(e2, t, t + 30001, false))[0];
    json::Value pv;
    json::parse(ping, pv);
    e2.on_ws_message("{\"id\":" + std::to_string(pv.get("id")->as_int()) + ",\"type\":\"pong\"}", t + 30500, WALL);
    ticks(e2, t + 31000, t + 50000, false);
    CHECK(e2.online());

    /* stopped by the launcher (freeze) for a minute: reconnect at once, no back-off */
    Engine f(Settings(), creds(), {});
    t = bring_online(f);
    f.take_actions();
    f.on_tick(t + 500, WALL);
    f.on_tick(t + 60500, WALL);
    auto a = f.take_actions();
    CHECK(has(a, Action::Kind::WsClose));
    CHECK(has(a, Action::Kind::WsOpen));
    CHECK(f.view(t, WALL).conn == Conn::Reconnecting);

    /* sign out: revoke on the server first; unreachable keeps the sign-in and says so */
    Engine g(Settings(), creds(), {});
    t = bring_online(g);
    g.take_actions();
    g.on_command(Command::of(Command::Kind::SignOut), t);
    a = g.take_actions();
    CHECK(a.size() == 1 && a[0].kind == Action::Kind::HttpPost && a[0].tag == HttpTag::Revoke);
    CHECK_EQ(a[0].url, std::string("http://homeassistant.local:8123/auth/revoke"));
    CHECK_EQ(a[0].body, std::string("token=REFRESH-SECRET"));
    CHECK(g.view(t, WALL).signing_out);
    g.on_command(Command::of(Command::Kind::SignOut), t); /* not twice */
    CHECK(g.take_actions().empty());
    g.on_http(HttpTag::Revoke, 0, "", "Could not connect", t);
    CHECK(g.signed_in());
    CHECK(g.view(t, WALL).message_error);
    CHECK(!g.view(t, WALL).signing_out);
    g.on_command(Command::of(Command::Kind::SignOut), t);
    g.take_actions();
    g.on_http(HttpTag::Revoke, 200, "", "", t);
    a = g.take_actions();
    CHECK(!g.signed_in());
    CHECK(has(a, Action::Kind::DeleteCredentials));
    CHECK(has(a, Action::Kind::WsClose));
    CHECK(has(a, Action::Kind::HttpGet)); /* the Connect page checks the server again */
    CHECK(g.view(t, WALL).conn == Conn::SignedOut);
    CHECK(g.credentials().refresh_token.empty());

    /* not connected: a command says so instead of queueing */
    Engine h(Settings(), creds(), {});
    h.start(T0);
    h.take_actions();
    h.on_command(Command::of(Command::Kind::Toggle, "light.k"), T0);
    CHECK(sent(h.take_actions()).empty());
    CHECK(h.view(T0, WALL).message_error);
}

static void test_name_cache()
{
    /* offline start: the cached names, the entity id's words for an entity without one */
    Engine e(Settings(), creds(), {"light.k", "scene.evening"});
    e.set_cached_names({{"light.k", "Old kitchen name"}, {"light.not_a_favourite", "x"}});
    e.start(T0);
    e.take_actions();
    e.on_http(HttpTag::Refresh, 0, "", "Could not resolve host", T0);
    e.take_actions();
    View v = e.view(T0, WALL);
    CHECK(v.conn == Conn::Connecting);
    CHECK_EQ(v.favourites[0].name, std::string("Old kitchen name"));
    CHECK_EQ(v.favourites[1].name, std::string("evening"));
    /* online: the registry names an entity the cache does not know (saved once); a cached name stays until a state
     * gives a friendly name (no flip between the registry's and the state's names at each connect) */
    Engine o(Settings(), creds(), {"light.k", "scene.evening"});
    o.set_cached_names({{"light.k", "Old kitchen name"}});
    std::vector<std::string> first;
    const std::int64_t t = bring_online(o, &first);
    auto a = o.take_actions();
    CHECK(of_kind(a, Action::Kind::SaveNames).size() == 1);
    CHECK_EQ(o.cached_names().at("light.k"), std::string("Old kitchen name"));
    CHECK_EQ(o.cached_names().at("scene.evening"), std::string("Evening"));
    CHECK(o.cached_names().size() == 2); /* only favourites */
    /* a friendly name from the state wins, and is saved; the same name again saves nothing */
    o.on_ws_message("{\"id\":7,\"type\":\"event\",\"event\":{\"a\":{\"light.k\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"Kitchen ceiling\"},\"c\":\"x\",\"lc\":1}}}}",
                    t, WALL);
    a = o.take_actions();
    CHECK(of_kind(a, Action::Kind::SaveNames).size() == 1);
    CHECK_EQ(o.cached_names().at("light.k"), std::string("Kitchen ceiling"));
    o.on_ws_message("{\"id\":7,\"type\":\"event\",\"event\":{\"c\":{\"light.k\":{\"+\":{\"s\":\"off\"}}}}}", t, WALL);
    CHECK(of_kind(o.take_actions(), Action::Kind::SaveNames).empty());
    /* unstarred: gone from the cache */
    o.on_command(Command::of(Command::Kind::ToggleFavourite, "light.k"), t);
    a = o.take_actions();
    CHECK(of_kind(a, Action::Kind::SaveNames).size() == 1);
    CHECK(!o.cached_names().count("light.k"));
    /* starred: its name joins at once (the registry knows it) */
    o.on_command(Command::of(Command::Kind::ToggleFavourite, "switch.coffee"), t);
    o.take_actions();
    CHECK_EQ(o.cached_names().at("switch.coffee"), std::string("Coffee"));
}

void test_engine_group()
{
    test_name_cache();
    test_pairing();
    test_pairing_failures();
    test_connect_and_messages();
    test_backoff_and_states();
    test_tokens_and_refusals();
    test_ping_freeze_signout();
}
