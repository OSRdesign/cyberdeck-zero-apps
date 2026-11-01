/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: whole flows of the engine against the simulated Home Assistant (the render harness's worlds), with the
 * virtual clock moved by the test.
 */
#include "test_util.hpp"

#include "sim.hpp"

#include <algorithm>

using namespace ha;

namespace {

Credentials sim_creds()
{
    Credentials c;
    c.server = "http://homeassistant.local:8123";
    c.client_id = "http://192.168.1.42:41234/";
    c.refresh_token = "sim-refresh-1";
    return c;
}

const EntityRow *row(const std::vector<EntityRow> &rows, const std::string &id)
{
    for (const EntityRow &r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

/* no token may reach the pages */
bool view_has_token(const View &v)
{
    auto bad = [](const std::string &s) { return s.find("sim-access") != std::string::npos || s.find("sim-refresh") != std::string::npos || s.find("sim-code") != std::string::npos; };
    if (bad(v.pair_status) || bad(v.qr_url) || bad(v.message) || bad(v.server_url) || bad(v.user)) return true;
    for (const EntityRow &r : v.favourites)
        if (bad(r.name) || bad(r.value)) return true;
    return false;
}

} // namespace

void test_sim_group()
{
    /* first run: pairing by the phone, then Home */
    {
        SimBackend b(sim_world("connect"), Settings(), Credentials(), {}, 1.0, false);
        b.advance(200);
        View v = b.snapshot();
        CHECK(!v.signed_in);
        CHECK(!v.qr_url.empty());
        CHECK_EQ(v.pair_status, std::string("Waiting for the phone..."));
        CHECK(b.listening());
        b.phone("ok");
        b.advance(500);
        v = b.snapshot();
        CHECK(v.signed_in);
        CHECK(v.paired_now);
        CHECK_EQ(v.pair_status, std::string("Connected as deck"));
        CHECK(v.conn == Conn::Connected);
        CHECK(b.credentials_file());
        CHECK_EQ(b.saved_credentials().refresh_token, std::string("sim-refresh-1"));
        CHECK_EQ(b.saved_credentials().client_id, std::string("http://192.168.1.42:41234/"));
        CHECK(!b.listening());
        CHECK(v.rooms.size() == 6); /* five areas + No room (the guest mode switch) */
        CHECK(!view_has_token(v));
        CHECK(b.world().exchanges == 1);
        CHECK(b.world().refreshes == 0); /* the fresh token from the exchange was used */
    }
    /* the phone's login refused, and a wrong code */
    {
        SimBackend b(sim_world("connect"), Settings(), Credentials(), {}, 1.0, false);
        b.advance(200);
        b.phone("refused");
        b.advance(200);
        View v = b.snapshot();
        CHECK(!v.signed_in);
        CHECK(v.pair_tone == Tone::Error);
        CHECK(v.pair_status.find("access_denied") != std::string::npos);
        b.phone("badcode");
        b.advance(200);
        v = b.snapshot();
        CHECK(v.pair_status.find("Invalid code") != std::string::npos);
        CHECK(!b.credentials_file());
    }
    /* signed in: Home with favourites, a room, commands and their states */
    {
        SimBackend b(sim_world("home"), Settings(), sim_creds(),
                     {"light.living_ceiling", "scene.evening", "sensor.living_temperature", "lock.front_door", "cover.garage_door"},
                     1.0, false);
        b.advance(300);
        View v = b.snapshot();
        CHECK(v.conn == Conn::Connected);
        CHECK(b.world().refreshes == 1);
        CHECK(v.favourites.size() == 5);
        CHECK_EQ(row(v.favourites, "light.living_ceiling")->value, std::string("On"));
        CHECK_EQ(row(v.favourites, "sensor.living_temperature")->value, std::string("21.5 C"));
        CHECK_EQ(row(v.favourites, "lock.front_door")->value, std::string("Locked"));
        CHECK_EQ(row(v.favourites, "cover.garage_door")->value, std::string("Closed"));
        CHECK(row(v.favourites, "cover.garage_door")->action == RowAction::Open);
        CHECK(v.rooms.size() == 6);
        /* the Living room: the climate via its device, the media player left out, hidden / diagnostic left out */
        b.send(Command::set_view(true, "living_room", ""));
        b.advance(200);
        v = b.snapshot();
        CHECK(v.room.size() == 8);
        CHECK(row(v.room, "climate.thermostat"));
        CHECK_EQ(row(v.room, "climate.thermostat")->value, std::string("Heat, 21.4 C"));
        CHECK(!row(v.room, "media_player.tv"));
        CHECK_EQ(row(v.room, "binary_sensor.living_window")->value, std::string("Closed"));
        /* toggle the TV plug: "...", then Off */
        b.send(Command::of(Command::Kind::Toggle, "switch.tv_plug"));
        v = b.snapshot();
        CHECK(row(v.room, "switch.tv_plug")->pending);
        b.advance(200);
        v = b.snapshot();
        CHECK(!row(v.room, "switch.tv_plug")->pending);
        CHECK_EQ(row(v.room, "switch.tv_plug")->value, std::string("Off"));
        /* run the scene: Done for a moment */
        b.send(Command::of(Command::Kind::Run, "scene.evening"));
        b.advance(100);
        CHECK(row(b.snapshot().room, "scene.evening")->done);
        b.advance(2000);
        CHECK(!row(b.snapshot().room, "scene.evening")->done);
        /* the light's brightness */
        b.send(Command::set_view(true, "living_room", "light.living_ceiling"));
        b.send(Command::of(Command::Kind::SetBrightness, "light.living_ceiling", "", 30));
        b.advance(200);
        v = b.snapshot();
        CHECK(v.detail.dimmable);
        CHECK(v.detail.brightness_pct == 30);
        CHECK(std::find(b.world().calls.begin(), b.world().calls.end(), "light.turn_on light.living_ceiling 30") != b.world().calls.end());
        /* the lock: the call goes out (the page asked first); a jammed one names the reason */
        b.send(Command::of(Command::Kind::Service, "lock.front_door", "unlock"));
        b.advance(200);
        CHECK_EQ(row(b.snapshot().favourites, "lock.front_door")->value, std::string("Unlocked"));
        b.send(Command::of(Command::Kind::Service, "lock.back_door", "unlock"));
        b.advance(200);
        v = b.snapshot();
        CHECK(v.message_error);
        CHECK_EQ(v.message, std::string("Could not unlock Back door: Lock jammed"));
        /* someone at the wall switch */
        b.server_set_state("light.living_ceiling", "off");
        b.advance(100);
        CHECK_EQ(row(b.snapshot().favourites, "light.living_ceiling")->value, std::string("Off"));
        /* star from the entity page: saved and subscribed */
        b.send(Command::set_view(false, "", "switch.coffee_machine"));
        b.send(Command::of(Command::Kind::ToggleFavourite, "switch.coffee_machine"));
        b.advance(200);
        CHECK(b.saved_favourites().size() == 6);
        CHECK(row(b.snapshot().favourites, "switch.coffee_machine"));
        CHECK_EQ(row(b.snapshot().favourites, "switch.coffee_machine")->value, std::string("Off"));
        /* the link drops: Reconnecting, values greyed; back after 2 s with the same token */
        const int refreshes = b.world().refreshes;
        b.drop_link();
        v = b.snapshot();
        CHECK(v.conn == Conn::Reconnecting);
        CHECK(row(v.favourites, "light.living_ceiling")->muted);
        CHECK_EQ(row(v.favourites, "light.living_ceiling")->value, std::string("Off"));
        b.advance(2500);
        CHECK(b.snapshot().conn == Conn::Connected);
        CHECK(b.world().refreshes == refreshes);
        /* an access token Home Assistant no longer takes (older than a minute): one refresh, connected again */
        b.advance(61000);
        CHECK(b.snapshot().conn == Conn::Connected); /* the pings kept the link */
        b.world().reject_access = true;
        b.drop_link();
        b.advance(3000);
        CHECK(b.snapshot().conn == Conn::Connected);
        CHECK(b.world().refreshes == refreshes + 1);
        CHECK(b.snapshot().signed_in);
        /* sign out: revoked, the file gone, the Connect page */
        b.send(Command::of(Command::Kind::SignOut));
        b.advance(300);
        v = b.snapshot();
        CHECK(!v.signed_in);
        CHECK(!b.credentials_file());
        CHECK(b.world().revokes == 1);
        CHECK(!v.qr_url.empty());
        for (const std::string &m : b.world().sent) /* tokens only ever in the auth message */
            if (m.find("sim-access") != std::string::npos) CHECK(m.rfind("{\"type\":\"auth\",", 0) == 0);
    }
    /* revoked in Home Assistant while the deck was away: refused at the refresh, back to the Connect page */
    {
        SimWorld w = sim_world("home");
        w.refuse_refresh = true;
        SimBackend b(w, Settings(), sim_creds(), {"light.kitchen"}, 1.0, false);
        b.advance(300);
        View v = b.snapshot();
        CHECK(!v.signed_in);
        CHECK(!b.credentials_file());
        CHECK(v.pair_tone == Tone::Error);
        CHECK(b.world().refreshes == 1); /* never retried */
        b.advance(10000);
        CHECK(b.world().refreshes == 1);
    }
    /* the server goes away: Reconnecting, then Offline after three failed attempts (2 + 4 + 8 s) */
    {
        SimBackend b(sim_world("home"), Settings(), sim_creds(), {"light.kitchen"}, 1.0, false);
        b.advance(300);
        b.world().reachable = false;
        b.drop_link();
        CHECK(b.snapshot().conn == Conn::Reconnecting);
        b.advance(5000);
        CHECK(b.snapshot().conn == Conn::Reconnecting);
        b.advance(10000);
        CHECK(b.snapshot().conn == Conn::Offline);
        CHECK(b.snapshot().last_update > 0);
        b.world().reachable = true;
        b.advance(20000); /* the next try is at most 16 s away */
        CHECK(b.snapshot().conn == Conn::Connected);
    }
    /* a sign-out that cannot reach the server keeps the sign-in */
    {
        SimBackend b(sim_world("home"), Settings(), sim_creds(), {}, 1.0, false);
        b.advance(300);
        b.world().reachable = false;
        b.send(Command::of(Command::Kind::SignOut));
        b.advance(100);
        View v = b.snapshot();
        CHECK(v.signed_in);
        CHECK(v.message.find("Sign out failed") == 0);
        CHECK(b.credentials_file());
    }
    /* the worlds of the harness all start */
    for (const char *name : {"connect", "connect-unreachable", "home", "empty", "big", "connecting", "reconnecting"}) {
        auto b = make_demo_backend(name);
        b->advance(500);
        CHECK(b->version() > 0);
    }
    {
        auto b = make_demo_backend("big");
        b->advance(300);
        b->send(Command::set_view(true, "workshop", ""));
        b->advance(300);
        CHECK(b->snapshot().room.size() == 300);
        CHECK(b->engine().subscribed().size() >= 300);
    }
    {
        /* a start without a connection: the favourites keep the names cached by an earlier session */
        auto b = make_demo_backend("connecting");
        b->advance(500);
        View v = b->snapshot();
        CHECK(v.conn == Conn::Connecting);
        CHECK(row(v.favourites, "light.living_ceiling") && row(v.favourites, "light.living_ceiling")->name == "Ceiling light");
        CHECK(row(v.favourites, "lock.front_door")->name == "Front door");
        /* back online, the cache is written from Home Assistant's names */
        b->world().reachable = true;
        b->advance(20000);
        CHECK(b->snapshot().conn == Conn::Connected);
        CHECK(b->saved_names().size() == 5);
        CHECK(b->saved_names().at("light.living_ceiling") == "Ceiling light");
    }
    {
        auto b = make_demo_backend("connect-unreachable");
        b->advance(300);
        CHECK(b->snapshot().pair_status.find("Cannot reach Home Assistant") == 0);
    }
}
