/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: what the pages show (View, built by the engine and copied to the LVGL thread) and what they
 * ask for (Command). No LVGL.
 */
#pragma once

#include "model.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ha {

/* The connection line: Connected (green), Connecting / Reconnecting (amber), Offline (red). SignedOut: the Connect page. */
enum class Conn { SignedOut, Connecting, Connected, Reconnecting, Offline };

struct EntityRow {
    std::string id, name, value;
    Domain domain = Domain::Other;
    RowAction action = RowAction::Info;
    bool muted = false;   /* value in grey: unavailable / unknown, not loaded yet, or offline (last value kept) */
    bool pending = false; /* a command was sent, the new state has not come back yet ("...") */
    bool done = false;    /* a scene or script just ran ("Done") */
    bool running = false; /* a script is running (the dot in blue) */
};

struct RoomRow {
    std::string id; /* "" = No room */
    std::string name;
    int count = 0;
};

struct Detail {
    bool known = false; /* the entity is in the registry or has a state */
    std::string id, name, room, value;
    Domain domain = Domain::Other;
    RowAction action = RowAction::Info;
    bool muted = false;
    bool pending = false;
    std::vector<std::pair<std::string, std::string>> info; /* read-only rows after the state */
    bool dimmable = false;  /* a light with brightness: the slider row */
    int brightness_pct = 0; /* 0..100 (0 = off) */
    bool on = false;
    bool confirm = false;   /* a lock, or a garage-type cover: actions ask first */
    bool favourite = false;
};

enum class Tone { Normal, Warn, Error };

struct View {
    bool signed_in = false;
    Conn conn = Conn::SignedOut;
    std::string server_display; /* "homeassistant.local:8123" */
    std::string server_host;    /* "homeassistant.local" (the Connect page's row; its address line has the rest) */
    std::string server_url;     /* "http://homeassistant.local:8123" */
    std::int64_t last_update = 0; /* epoch of the last state received */
    std::string user, ha_version;
    bool registry_loaded = false;

    /* the Connect page */
    std::string qr_url;      /* "" while there is no code to show */
    std::string pair_status; /* "Waiting for the phone...", "Connected as deck", a failure in plain words */
    Tone pair_tone = Tone::Normal;
    bool paired_now = false; /* the sign-in of this run finished and the user is known ("Connected as ...") */

    std::vector<EntityRow> favourites;
    std::vector<RoomRow> rooms;
    bool room_shown = false;
    std::string room_id, room_name;
    std::vector<EntityRow> room;
    std::string entity_id;
    Detail detail;

    /* a short message for the status line (a command that failed, a sign-out that could not reach the server) */
    std::string message;
    bool message_error = false;
    std::uint64_t message_seq = 0;
    bool signing_out = false;
};

struct Command {
    enum class Kind {
        SetView,         /* flag: a room is shown (a = its id, "" = No room); b: the entity page shown ("" none) */
        ToggleFavourite, /* a: entity */
        Toggle,          /* a: entity (light, switch, input_boolean) */
        Run,             /* a: entity (scene, script) */
        SetBrightness,   /* a: light, value 0..100 (0 = off) */
        Service,         /* a: entity, b: service of its domain (lock, unlock, open_cover, close_cover) */
        SignOut,
        SetServer,       /* a: the address as typed */
    };
    Kind kind = Kind::SetView;
    std::string a, b;
    int value = 0;
    bool flag = false;

    static Command set_view(bool room_shown, const std::string &room, const std::string &entity)
    {
        Command c;
        c.kind = Kind::SetView;
        c.flag = room_shown;
        c.a = room;
        c.b = entity;
        return c;
    }
    static Command of(Kind k, const std::string &a = std::string(), const std::string &b = std::string(), int v = 0)
    {
        Command c;
        c.kind = k;
        c.a = a;
        c.b = b;
        c.value = v;
        return c;
    }
};

/* What the pages talk to: the real client (a connection thread) or the simulated Home Assistant of the tests and the
 * render harness. LVGL thread only. */
class Backend {
public:
    virtual ~Backend() = default;
    /* Bumped whenever the view changes. Cheap: called by the page timer. */
    virtual std::uint64_t version() = 0;
    virtual View snapshot() = 0;
    virtual void send(const Command &c) = 0;
    /* The page timer's tick (the simulated backend runs its world here; the real one does nothing). */
    virtual void poll() {}
};

} // namespace ha
