/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the client as a state machine, with no I/O of its own (tests/test_engine.cpp drives it with
 * scripted events; the connection thread, runner.hpp, executes its actions with libcurl and sockets; the simulated
 * Home Assistant of the render harness, sim.hpp, answers them in memory).
 *
 * Pairing (no credentials): GET <server>/auth/providers (unauthenticated, answers 200: proves Home Assistant is there
 * without a failed-login count) -> a listener on the deck's LAN address and a free port -> the QR code of
 * <server>/auth/authorize?client_id=http://<deck>:<port>/&redirect_uri=http://<deck>:<port>/callback&state=<random>
 * (Home Assistant accepts a client id on a local IP address and a redirect to the same host and port: spike probe 3) ->
 * the phone logs in and comes back to the listener with a one-time code -> POST <server>/auth/token
 * (authorization_code) -> an access token (30 min) and a refresh token, kept with the client id in the credentials file.
 *
 * Connecting: a fresh access token first (POST /auth/token, grant refresh_token, the same client id) unless the one in
 * memory has more than a minute left -> ws://<server>/api/websocket: auth_required -> auth -> auth_ok -> features
 * (coalesced messages), config (temperature unit), current user, areas, devices, entity registry for display -> one
 * subscribe_entities for the favourites and the room or entity shown. While connected: a ping after 30 s without any
 * message, the link dropped after 15 s without an answer; the access token refreshed 2 minutes before it ends.
 * A dropped link retries after 2, 4, 8, 16, 32, then 60 s.
 *
 * Failed logins count toward Home Assistant's IP ban (when the owner enabled it): an auth_invalid with a token older
 * than a minute is retried once with a new token; any other refusal (auth_invalid again, a 4xx from /auth/token) stops,
 * revokes what is left and goes back to the Connect page. Tokens never leave this object except in the actions'
 * bodies; nothing here logs.
 */
#pragma once

#include "icons.hpp"
#include "model.hpp"
#include "text.hpp"
#include "view.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ha {

struct Settings {
    std::string server = kDefaultServer; /* as typed (shown) */
    bool qr_use_ip = false; /* the QR code names Home Assistant by its IP address (a phone that cannot resolve .local) */
};

struct Credentials {
    std::string server;        /* base URL the sign-in was made with */
    std::string client_id;     /* exactly as at pairing: /auth/token compares it */
    std::string refresh_token; /* never logged */
    bool valid() const { return !server.empty() && !client_id.empty() && !refresh_token.empty(); }
};

enum class HttpTag { CheckServer, Exchange, Refresh, Revoke, RevokeQuiet };

struct Action {
    enum class Kind {
        HttpGet,           /* url, tag */
        HttpPost,          /* url, body (form), tag */
        WsOpen,            /* url */
        WsSend,            /* body (one text message) */
        WsClose,
        ListenStart,       /* host: Home Assistant's host (the deck's address is the one that reaches it) */
        ListenStop,
        SaveCredentials,   /* engine.credentials() */
        DeleteCredentials,
        SaveSettings,      /* engine.settings() */
        SaveFavourites,    /* engine.favourites() */
        SaveNames,         /* engine.cached_names(): the favourites' names, for a start without a connection */
        SaveLayout,        /* engine.layout_text(): areas, entities and icon rules, for a start without a connection */
    };
    Kind kind = Kind::WsClose;
    std::string url, body, host;
    HttpTag tag = HttpTag::CheckServer;
};

/* 2, 4, 8, 16, 32, then 60 s (failures = consecutive failed attempts, from 1). */
std::int64_t backoff_ms(int failures);

/* layout.json (and any file the app reads) at most 1 MB: a larger install is not cached */
constexpr std::size_t kMaxLayoutBytes = 1u << 20;

class Engine {
public:
    Engine(Settings settings, Credentials credentials, std::vector<std::string> favourites);

    void start(std::int64_t now);
    void on_command(const Command &c, std::int64_t now);
    /* status 0 = no answer (error says why) */
    void on_http(HttpTag tag, int status, const std::string &body, const std::string &error, std::int64_t now);
    void on_ws_open(bool ok, const std::string &why, std::int64_t now);
    void on_ws_message(const std::string &text, std::int64_t now, std::int64_t wall);
    void on_ws_closed(const std::string &why, std::int64_t now);
    /* the listener: the deck's LAN address and port, Home Assistant's IPv4 address, a random state string */
    void on_listen(bool ok, const std::string &lan_ip, int port, const std::string &ha_ip, const std::string &state,
                   const std::string &why, std::int64_t now);
    /* a request reached the listener (path without the query, the decoded query) */
    void on_callback(const std::string &path, const std::map<std::string, std::string> &query, std::int64_t now);
    void on_tick(std::int64_t now, std::int64_t wall);

    std::vector<Action> take_actions();
    /* the next time on_tick() has something to do (now + 1 s at most while connected) */
    std::int64_t next_deadline(std::int64_t now) const;
    std::uint64_t version() const { return version_; }
    View view(std::int64_t now, std::int64_t wall) const;

    const Settings &settings() const { return settings_; }
    const Credentials &credentials() const { return creds_; }
    const std::vector<std::string> &favourites() const { return favourites_; }
    /* The favourites' names as last seen (entity id -> friendly name): shown while the registry and the states have not
     * arrived (a start without a connection). Set before start(); kept up to date as names arrive. */
    void set_cached_names(std::map<std::string, std::string> names) { names_ = std::move(names); }
    const std::map<std::string, std::string> &cached_names() const { return names_; }
    /* The layout of an earlier session (store::load_layout): the tabs and tiles show it, greyed, until Home Assistant
     * answers. Set before start(). */
    void set_cached_layout(std::vector<Area> areas, std::vector<RegEntry> entries, IconRules icons);
    /* layout.json's text for the registry and icon rules as they are now ("" while there is no registry); at most
     * kMaxLayoutBytes (a larger install is not cached). */
    std::string layout_text() const;
    const IconRules &icon_rules() const { return icons_; }
    const Model &model() const { return model_; }
    bool signed_in() const { return creds_.valid(); }
    bool online() const { return st_ == St::Online; }
    bool listening() const { return st_ == St::PairWait || st_ == St::PairExchange; }
    /* for the tests: the subscription's entity ids, the client id of the pairing in progress */
    const std::set<std::string> &subscribed() const { return sub_set_; }
    const std::string &pairing_client_id() const { return pair_client_id_; }

private:
    enum class St { PairCheck, PairCheckWait, PairListen, PairWait, PairExchange, Refresh, Open, Auth, Online, Backoff };
    enum class Req { Features, Config, User, Areas, Devices, Entities, Subscribe, Unsubscribe, Call, Ping, IconsComponent,
                     IconsEntity };
    struct Pending {
        Req req = Req::Call;
        std::string entity, service;
    };

    void push(Action a) { actions_.push_back(std::move(a)); }
    void take_token(const std::string &access, const json::Value &answer, std::int64_t now);
    void changed() { ++version_; }
    void say(const std::string &text, bool error);
    Server server() const;
    std::string base() const;

    void begin_pairing(std::int64_t now, const std::string &notice = std::string(), Tone tone = Tone::Normal);
    void begin_connect(std::int64_t now);
    void open_link(std::int64_t now);
    void fail_connect(std::int64_t now, const std::string &why);
    void reset_session();
    void signed_out(std::int64_t now, const std::string &notice, Tone tone, bool revoke);

    int send(const std::string &type, const std::string &fields, Req req, const std::string &entity = std::string(),
             const std::string &service = std::string());
    void handle(const json::Value &msg, std::int64_t now, std::int64_t wall);
    void on_result(const json::Value &msg, const Pending &p, std::int64_t now);
    void update_subscription();
    void call_service(const std::string &entity, const std::string &domain, const std::string &service,
                      const std::string &data, bool pending, std::int64_t now);
    std::set<std::string> wanted_set() const;
    void update_name_cache();
    void send_icon_requests();
    void save_layout();
    std::string name_of(const std::string &id) const;
    EntityRow row(const std::string &id, std::int64_t now) const;

    Settings settings_;
    Credentials creds_;
    std::vector<std::string> favourites_;
    std::map<std::string, std::string> names_;
    Model model_;

    St st_ = St::PairCheck;
    std::uint64_t version_ = 1;
    std::vector<Action> actions_;

    /* pairing */
    std::string pair_client_id_, pair_state_, qr_url_, pair_status_;
    Tone pair_tone_ = Tone::Normal;
    std::int64_t pair_since_ = 0;
    bool pair_hint_ = false;
    bool pair_notice_ = false; /* the status line explains why we are here (signed out, refused): kept until the phone answers */
    bool paired_now_ = false;
    int pair_failures_ = 0;

    /* tokens: in memory only */
    std::string access_token_;
    std::int64_t token_expires_ = 0, token_obtained_ = 0;
    std::int64_t token_margin_ = 60000, token_early_ = 120000; /* see take_token() */
    bool refresh_in_flight_ = false;
    std::int64_t refresh_retry_at_ = 0;
    bool auth_retry_used_ = false;

    /* the link */
    int failures_ = 0;
    bool ever_connected_ = false;
    std::int64_t retry_at_ = 0, auth_deadline_ = 0, last_rx_ = 0, ping_at_ = 0, last_tick_ = 0;
    bool awaiting_pong_ = false;
    std::string last_error_;
    bool signing_out_ = false;

    /* the session */
    int next_id_ = 1;
    std::map<int, Pending> pending_req_;
    int registry_wait_ = 0;
    bool registry_ready_ = false;
    bool registry_ever_ = false; /* the registry arrived once (kept, greyed, while offline) */
    int sub_id_ = 0;
    std::set<std::string> sub_set_;
    std::string user_, ha_version_, temp_unit_ = "C", temp_unit_raw_ = "\xC2\xB0" "C";
    IconRules icons_;
    std::string layout_saved_; /* the text of the last SaveLayout (or of the file read at start) */
    std::int64_t last_update_ = 0;
    std::map<std::string, std::int64_t> pending_until_; /* entity -> "..." until */
    std::map<std::string, std::int64_t> done_until_;    /* entity -> "Done" until */

    /* what the pages show */
    bool view_room_ = false;
    std::string view_room_id_, view_entity_;

    std::string message_;
    bool message_error_ = false;
    std::uint64_t message_seq_ = 0;
};

} // namespace ha
