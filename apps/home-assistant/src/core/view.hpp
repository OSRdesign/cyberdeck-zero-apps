/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: what the pages show (View, built by the engine and copied to the LVGL thread) and what they
 * ask for (Command). No LVGL.
 */
#pragma once

#include "lovelace.hpp"
#include "model.hpp"
#include "tiles.hpp"

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

    /* the tile (tiles.hpp): its second line, glyph and colours, what its icon does */
    std::string tile_value;      /* "On, 72 %", "21.5 °C", "Unavailable", "..." (pending) */
    std::uint32_t glyph = 0;     /* MDI codepoint; 0 when the font's table knows none of its names */
    std::uint32_t glyph_rgb = rgb::kInactive;
    std::uint32_t circle_rgb = rgb::kCircle;
    bool active = false;         /* the circle tinted */
    bool unavailable = false;    /* the tile dimmed */
    TileAction tile_action = TileAction::Details;
    bool confirm = false;        /* the icon's action asks first (a lock, a garage / gate / door cover) */
    bool dimmable = false;       /* a light with brightness: a slider row under its tile */
    int brightness_pct = 0;      /* 0..100 (0 = off) */
    bool on = false;             /* on / open / unlocked: which way the action goes */
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
    /* the page's top (study 053 section 7.6): the tile's glyph and colours, its word, "Living room - changed 19:58" */
    EntityRow tile;
    std::string meta;
};

enum class Tone { Normal, Warn, Error };

/* ------------------------------------------------------------------ the stored dashboard (0.3.0, study 053 P2) */

/* One row of an entities card, as the page draws it. */
struct DashRowView {
    DashRow::Kind kind = DashRow::Kind::Entity;
    std::string label;     /* Section: its label; Other: the row's name, else its type ("weblink") */
    EntityRow e;           /* Entity: the name (the row's override), glyph, colours, what the icon does */
    std::string word;      /* Entity: the state word ("On", "21.5 °C", "Home", "Not available", "...") */
    bool toggle = false;   /* Entity: a switch (light, switch, fan, input_boolean) */
    bool available = true; /* Entity: Home Assistant has it (false: "Not available", greyed) */
    bool has_state = false; /* Entity: its state arrived (else the word "..." or "Offline", never a switch) */
    TapAction tap = TapAction::Default; /* what a tap on the name does */
    bool operator==(const DashRowView &o) const
    {
        return kind == o.kind && label == o.label && word == o.word && toggle == o.toggle && available == o.available &&
               has_state == o.has_state &&
               tap == o.tap && e.id == o.e.id && e.name == o.e.name && e.glyph == o.e.glyph &&
               e.glyph_rgb == o.e.glyph_rgb && e.muted == o.e.muted && e.pending == o.e.pending && e.done == o.e.done &&
               e.on == o.e.on && e.unavailable == o.e.unavailable && e.confirm == o.e.confirm &&
               e.tile_action == o.e.tile_action;
    }
    bool operator!=(const DashRowView &o) const { return !(*this == o); }
};

struct ForecastCol {
    std::string day;         /* "Mon" */
    std::uint32_t glyph = 0; /* the condition's MDI codepoint */
    std::string high, low;   /* "14°", "6°" ("" unknown) */
    bool operator==(const ForecastCol &o) const { return day == o.day && glyph == o.glyph && high == o.high && low == o.low; }
};

/* One card of the view shown. */
struct DashCardView {
    DashCard::Kind kind = DashCard::Kind::Unsupported;
    std::string type, title;
    /* entities */
    std::vector<DashRowView> rows;
    bool header_toggle = false, header_on = false, header_pending = false;
    std::vector<std::string> toggle_ids; /* what the header toggle switches */
    /* sensor and weather: the entity (name, glyph, colours) */
    EntityRow e;
    std::string value, unit;  /* sensor: "21.5", "°C"; weather: the temperature "14.2", "°C" */
    bool graph = false;       /* sensor: graph: line */
    std::vector<int> spark;   /* sensor: the points 0..1000 (-1 none); empty: no history (the value only) */
    std::string condition;    /* weather: "Partly cloudy" */
    std::uint32_t big_glyph = 0; /* weather: the condition's glyph */
    bool forecast = false;    /* weather: show_forecast */
    std::vector<ForecastCol> days;
};

/* A view of the dashboard that gets a tab. */
struct DashTab {
    int view = 0;            /* its index in the dashboard */
    std::string title;       /* "" when it has none */
    std::uint32_t glyph = 0; /* its icon (0 none) */
};

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
    std::vector<Tab> tabs; /* Favourites, the areas with entities, All */
    bool room_shown = false;
    std::string room_id, room_name;
    std::vector<EntityRow> room;
    std::string entity_id;
    Detail detail;

    /* the stored dashboard (0.3.0): its views' tabs come first; the cards of the view shown (dash_view) */
    bool dash = false;            /* a dashboard with supported cards is shown */
    std::string dash_key, dash_title;
    std::vector<DashTab> dash_tabs;
    int dash_view = -1;           /* the view whose cards dash_cards holds (-1: none yet) */
    bool dash_panel = false;      /* that view is a panel (one card, full width) */
    std::vector<DashCardView> dash_cards;
    /* Settings > Dashboard: the setting ("auto", "off", a key), the offered dashboards (key, title), Automatic's */
    std::string dash_setting = kDashAuto;
    std::vector<std::pair<std::string, std::string>> dash_choices;
    std::string dash_auto_title;

    /* a short message for the status line (a command that failed, a sign-out that could not reach the server) */
    std::string message;
    bool message_error = false;
    std::uint64_t message_seq = 0;
    bool signing_out = false;
};

struct Command {
    enum class Kind {
        SetView,         /* flag: a room is shown (a = its id, "" = No room); b: the entity page shown ("" none);
                            value: the dashboard view shown (-1 none) */
        ToggleFavourite, /* a: entity */
        Toggle,          /* a: entity (light, switch, input_boolean) */
        Run,             /* a: entity (scene, script) */
        SetBrightness,   /* a: light, value 0..100 (0 = off) */
        Service,         /* a: entity, b: service of its domain (lock, unlock, open_cover, close_cover) */
        SignOut,
        SetServer,       /* a: the address as typed */
        SetDashboard,    /* a: "auto", "off" or a dashboard's key (Settings > Dashboard) */
        SetGroup,        /* ids: the entities of an entities card's header toggle; flag: on */
    };
    Kind kind = Kind::SetView;
    std::string a, b;
    int value = 0;
    bool flag = false;
    std::vector<std::string> ids;

    static Command set_view(bool room_shown, const std::string &room, const std::string &entity, int dash_view = -1)
    {
        Command c;
        c.kind = Kind::SetView;
        c.flag = room_shown;
        c.a = room;
        c.b = entity;
        c.value = dash_view;
        return c;
    }
    static Command set_group(std::vector<std::string> ids, bool on)
    {
        Command c;
        c.kind = Kind::SetGroup;
        c.ids = std::move(ids);
        c.flag = on;
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
